#include "IngestionService.h"

#include <atomic>
#include <exception>
#include <stdexcept>
#include <utility>

namespace labscope::ingest
{

struct IngestionService::DeviceLane
{
    explicit DeviceLane(std::size_t capacity) : queue(capacity)
    {
    }

    BoundedDeviceQueue queue;
    bool inFlight{false};
    bool readyQueued{false};
};

struct IngestionService::DispatchItem
{
    std::string key;
    QueuedTelemetry telemetry;
};

IngestionService::IngestionService(std::size_t capacity,
                                   std::size_t workerCount,
                                   std::size_t maxBatchSize,
                                   BatchHandler batchHandler)
    : capacity_(capacity),
      workerCount_(workerCount),
      maxBatchSize_(maxBatchSize),
      batchHandler_(std::move(batchHandler))
{
    if (capacity_ == 0 || workerCount_ == 0 || maxBatchSize_ == 0 || !batchHandler_)
        throw std::invalid_argument("ingestion service requires positive limits and a batch handler");
}

IngestionService::~IngestionService()
{
    stop();
}

void IngestionService::start()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (started_)
        return;
    started_ = true;
    accepting_ = true;
    stopping_ = false;
    try
    {
        workers_.reserve(workerCount_);
        for (std::size_t index = 0; index < workerCount_; ++index)
            workers_.emplace_back([this] { workerLoop(); });
    }
    catch (...)
    {
        accepting_ = false;
        stopping_ = true;
        condition_.notify_all();
        auto workers = std::move(workers_);
        lock.unlock();
        for (auto &worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }
        lock.lock();
        started_ = false;
        throw;
    }
}

void IngestionService::stop()
{
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_)
            return;
        accepting_ = false;
        stopping_ = true;
        condition_.notify_all();
        workers.swap(workers_);
    }
    for (auto &worker : workers)
    {
        if (worker.joinable())
            worker.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    lanes_.clear();
    readyDevices_.clear();
}

QueueResult IngestionService::submit(telemetry::TelemetryEnvelope envelope,
                                     CompletionCallback completion)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_)
    {
        ++rejectedWhileStopping_;
        return QueueResult::Stopping;
    }
    if (pending_ >= capacity_)
    {
        ++queueFull_;
        return QueueResult::Full;
    }

    std::string key = envelope.siteId;
    key.push_back('\x1f');
    key += envelope.deviceId;
    auto found = lanes_.find(key);
    if (found == lanes_.end())
    {
        found = lanes_.emplace(key, std::make_shared<DeviceLane>(capacity_)).first;
    }
    QueuedTelemetry queued{std::move(envelope), std::move(completion)};
    if (!found->second->queue.tryPush(std::move(queued)))
    {
        ++queueFull_;
        return QueueResult::Full;
    }

    ++pending_;
    ++accepted_;
    scheduleLane(key, found->second);
    condition_.notify_one();
    return QueueResult::Accepted;
}

IngestionQueueStats IngestionService::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {pending_, capacity_, accepted_, queueFull_, rejectedWhileStopping_};
}

void IngestionService::scheduleLane(const std::string &key,
                                    const std::shared_ptr<DeviceLane> &lane)
{
    if (!lane->inFlight && !lane->readyQueued && !lane->queue.empty())
    {
        lane->readyQueued = true;
        readyDevices_.push_back(key);
    }
}

void IngestionService::workerLoop()
{
    while (true)
    {
        std::vector<DispatchItem> pendingBatch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return !readyDevices_.empty() || (stopping_ && pending_ == 0);
            });
            if (stopping_ && pending_ == 0)
                return;

            pendingBatch.reserve(maxBatchSize_);
            while (!readyDevices_.empty() && pendingBatch.size() < maxBatchSize_)
            {
                auto key = std::move(readyDevices_.front());
                readyDevices_.pop_front();
                auto laneIt = lanes_.find(key);
                if (laneIt == lanes_.end())
                    continue;
                auto &lane = laneIt->second;
                lane->readyQueued = false;
                if (lane->inFlight)
                    continue;

                QueuedTelemetry queued;
                if (!lane->queue.tryPop(queued))
                {
                    lanes_.erase(laneIt);
                    continue;
                }
                lane->inFlight = true;
                pendingBatch.push_back({std::move(key), std::move(queued)});
            }
        }

        if (pendingBatch.empty())
            continue;

        auto dispatched = std::make_shared<std::vector<DispatchItem>>(std::move(pendingBatch));
        std::vector<QueuedTelemetry> handlerBatch;
        handlerBatch.reserve(dispatched->size());
        for (const auto &item : *dispatched)
            handlerBatch.push_back(item.telemetry);

        auto called = std::make_shared<std::atomic<bool>>(false);
        BatchCompletionCallback completion =
            [this, dispatched, called](std::vector<IngestionResult> results) mutable {
                if (called->exchange(true))
                    return;
                complete(dispatched, std::move(results));
            };
        try
        {
            batchHandler_(std::move(handlerBatch), completion);
        }
        catch (const std::exception &error)
        {
            completion(std::vector<IngestionResult>(
                dispatched->size(),
                {IngestionStatus::RetryableError,
                 std::string("batch handler exception: ") + error.what()}));
        }
        catch (...)
        {
            completion(std::vector<IngestionResult>(
                dispatched->size(),
                {IngestionStatus::RetryableError, "unknown batch handler exception"}));
        }
    }
}

void IngestionService::complete(
    const std::shared_ptr<std::vector<DispatchItem>> &batch,
    std::vector<IngestionResult> results)
{
    if (results.size() != batch->size())
    {
        results.assign(batch->size(),
                       {IngestionStatus::RetryableError,
                        "batch handler returned a mismatched result count"});
    }

    for (std::size_t index = 0; index < batch->size(); ++index)
    {
        auto &callback = (*batch)[index].telemetry.completion;
        if (callback)
        {
            try
            {
                callback(std::move(results[index]));
            }
            catch (...)
            {
                // User callbacks must not terminate worker threads.
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &item : *batch)
        {
            auto laneIt = lanes_.find(item.key);
            if (laneIt != lanes_.end())
            {
                auto lane = laneIt->second;
                lane->inFlight = false;
                if (pending_ > 0)
                    --pending_;
                if (lane->queue.empty())
                    lanes_.erase(laneIt);
                else
                    scheduleLane(item.key, lane);
            }
            else if (pending_ > 0)
            {
                --pending_;
            }
        }
    }
    condition_.notify_all();
}

}  // namespace labscope::ingest
