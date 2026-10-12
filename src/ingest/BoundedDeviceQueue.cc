#include "BoundedDeviceQueue.h"

#include <stdexcept>
#include <utility>

namespace labscope::ingest
{

BoundedDeviceQueue::BoundedDeviceQueue(std::size_t capacity) : capacity_(capacity)
{
    if (capacity_ == 0)
        throw std::invalid_argument("device queue capacity must be greater than zero");
}

bool BoundedDeviceQueue::tryPush(QueuedTelemetry item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_)
        return false;
    queue_.push_back(std::move(item));
    return true;
}

bool BoundedDeviceQueue::tryPop(QueuedTelemetry &item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
        return false;
    item = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::size_t BoundedDeviceQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool BoundedDeviceQueue::empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

}  // namespace labscope::ingest
