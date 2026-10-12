#pragma once

#include "BoundedDeviceQueue.h"

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace labscope::ingest
{

using BatchCompletionCallback = std::function<void(std::vector<IngestionResult>)>;
using BatchHandler = std::function<void(std::vector<QueuedTelemetry>,
                                        BatchCompletionCallback)>;

struct IngestionQueueStats
{
    std::size_t depth{0};
    std::size_t capacity{0};
    uint64_t accepted{0};
    uint64_t queueFull{0};
    uint64_t rejectedWhileStopping{0};
};

class IngestionService final
{
  public:
    IngestionService(std::size_t capacity,
                     std::size_t workerCount,
                     std::size_t maxBatchSize,
                     BatchHandler batchHandler);
    ~IngestionService();

    IngestionService(const IngestionService &) = delete;
    IngestionService &operator=(const IngestionService &) = delete;

    void start();
    void stop();
    QueueResult submit(telemetry::TelemetryEnvelope envelope,
                       CompletionCallback completion);
    IngestionQueueStats stats() const;

  private:
    struct DeviceLane;
    struct DispatchItem;

    void workerLoop();
    void complete(const std::shared_ptr<std::vector<DispatchItem>> &batch,
                  std::vector<IngestionResult> results);
    void scheduleLane(const std::string &key, const std::shared_ptr<DeviceLane> &lane);

    const std::size_t capacity_;
    const std::size_t workerCount_;
    const std::size_t maxBatchSize_;
    BatchHandler batchHandler_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, std::shared_ptr<DeviceLane>> lanes_;
    std::deque<std::string> readyDevices_;
    std::size_t pending_{0};
    uint64_t accepted_{0};
    uint64_t queueFull_{0};
    uint64_t rejectedWhileStopping_{0};
    bool started_{false};
    bool accepting_{false};
    bool stopping_{false};
    std::vector<std::thread> workers_;
};

}  // namespace labscope::ingest
