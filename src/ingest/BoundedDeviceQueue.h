#pragma once

#include "IngestionTypes.h"

#include <cstddef>
#include <deque>
#include <mutex>

namespace labscope::ingest
{

class BoundedDeviceQueue final
{
  public:
    explicit BoundedDeviceQueue(std::size_t capacity);

    bool tryPush(QueuedTelemetry item);
    bool tryPop(QueuedTelemetry &item);
    std::size_t size() const;
    bool empty() const;

  private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<QueuedTelemetry> queue_;
};

}  // namespace labscope::ingest
