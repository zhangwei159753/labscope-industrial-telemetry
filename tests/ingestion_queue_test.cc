#include "ingest/BoundedDeviceQueue.h"
#include "ingest/IngestionService.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace labscope::ingest;

std::atomic<int> failures{0};

void expect(bool condition, const char *name)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << name << '\n';
    }
}

QueuedTelemetry item(std::string device, int64_t seq, CompletionCallback completion = {})
{
    QueuedTelemetry queued;
    queued.envelope.siteId = "plant-a";
    queued.envelope.deviceId = std::move(device);
    queued.envelope.seq = seq;
    queued.completion = std::move(completion);
    return queued;
}

QueueResult submitItem(IngestionService &service, QueuedTelemetry queued)
{
    return service.submit(std::move(queued.envelope), std::move(queued.completion));
}

void testBoundedDeviceQueueCapacityAndFifo()
{
    BoundedDeviceQueue queue(2);
    expect(queue.tryPush(item("pump-1", 10)), "first queued item accepted");
    expect(queue.tryPush(item("pump-1", 11)), "second queued item accepted");
    expect(!queue.tryPush(item("pump-1", 12)), "queue rejects item over capacity");
    expect(queue.size() == 2, "queue remains at configured capacity");

    QueuedTelemetry popped;
    expect(queue.tryPop(popped) && popped.envelope.seq == 10, "FIFO returns first sequence");
    expect(queue.tryPop(popped) && popped.envelope.seq == 11, "FIFO returns second sequence");
    expect(!queue.tryPop(popped), "empty queue returns false");
}

void testServiceSerializesEachDeviceAndCompletesExactlyOnce()
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::string, int64_t>> persisted;
    int completionCount = 0;

    IngestionService service(
        8,
        2,
        2,
        [&](std::vector<QueuedTelemetry> batch, BatchCompletionCallback done) {
            std::vector<IngestionResult> results(batch.size(),
                                                 {IngestionStatus::Persisted, {}});
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto &queued : batch)
                    persisted.emplace_back(queued.envelope.deviceId, queued.envelope.seq);
            }
            done(std::move(results));
        });

    service.start();
    expect(submitItem(service, item("pump-1", 1, [&](IngestionResult result) {
               std::lock_guard<std::mutex> lock(mutex);
               expect(result.status == IngestionStatus::Persisted, "first result is persisted");
               ++completionCount;
               changed.notify_all();
           })) == QueueResult::Accepted,
           "first device item accepted");
    expect(submitItem(service, item("pump-1", 2, [&](IngestionResult result) {
               std::lock_guard<std::mutex> lock(mutex);
               expect(result.status == IngestionStatus::Persisted, "second result is persisted");
               ++completionCount;
               changed.notify_all();
           })) == QueueResult::Accepted,
           "second same-device item accepted");
    expect(submitItem(service, item("pump-2", 1, [&](IngestionResult result) {
               std::lock_guard<std::mutex> lock(mutex);
               expect(result.status == IngestionStatus::Persisted, "other device result is persisted");
               ++completionCount;
               changed.notify_all();
           })) == QueueResult::Accepted,
           "different device item accepted");

    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(3), [&] { return completionCount == 3; });
        expect(completionCount == 3, "each accepted item receives exactly one completion");
        std::vector<int64_t> pump1;
        for (const auto &entry : persisted)
            if (entry.first == "pump-1")
                pump1.push_back(entry.second);
        expect(pump1 == std::vector<int64_t>({1, 2}), "same-device persistence remains ordered");
    }
    service.stop();
}

void testInFlightWorkCountsTowardGlobalCapacity()
{
    std::mutex mutex;
    std::condition_variable changed;
    BatchCompletionCallback delayedCompletion;
    bool handlerEntered = false;
    int completionCount = 0;

    IngestionService service(
        1,
        1,
        1,
        [&](std::vector<QueuedTelemetry>, BatchCompletionCallback done) {
            std::lock_guard<std::mutex> lock(mutex);
            delayedCompletion = std::move(done);
            handlerEntered = true;
            changed.notify_all();
        });
    service.start();
    expect(submitItem(service, item("pump-1", 1, [&](IngestionResult) {
               std::lock_guard<std::mutex> lock(mutex);
               ++completionCount;
               changed.notify_all();
           })) == QueueResult::Accepted,
           "first item enters bounded service");

    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(3), [&] { return handlerEntered; });
        expect(handlerEntered, "worker dispatches accepted item");
    }
    expect(submitItem(service, item("pump-1", 2)) == QueueResult::Full,
           "in-flight item consumes global capacity");

    BatchCompletionCallback finish;
    {
        std::lock_guard<std::mutex> lock(mutex);
        finish = std::move(delayedCompletion);
    }
    if (finish)
        finish({{IngestionStatus::Persisted, {}}});
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(3), [&] { return completionCount == 1; });
        expect(completionCount == 1, "delayed persistence completes once");
    }
    service.stop();
}
}  // namespace

int main()
{
    testBoundedDeviceQueueCapacityAndFifo();
    testServiceSerializesEachDeviceAndCompletesExactlyOnce();
    testInFlightWorkCountsTowardGlobalCapacity();
    if (failures.load() != 0)
    {
        std::cerr << failures.load() << " ingestion queue test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ingestion queue tests passed\n";
    return EXIT_SUCCESS;
}
