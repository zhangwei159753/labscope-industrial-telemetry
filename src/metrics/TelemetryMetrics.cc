#include "TelemetryMetrics.h"

namespace labscope::metrics
{

void TelemetryMetrics::recordReceived() noexcept { ++received_; }
void TelemetryMetrics::recordInvalid() noexcept { ++rejected_; }
void TelemetryMetrics::recordQueueFull() noexcept { ++queueFull_; }

void TelemetryMetrics::recordResult(IngestOutcome outcome,
                                    uint64_t queueToCommitMs) noexcept
{
    switch (outcome)
    {
        case IngestOutcome::Persisted:
            ++persisted_;
            break;
        case IngestOutcome::Duplicate:
            ++duplicates_;
            break;
        case IngestOutcome::Rejected:
            ++rejected_;
            break;
        case IngestOutcome::Retryable:
            ++retries_;
            return;
    }
    ++latencyCount_;
    latencySumMs_.fetch_add(queueToCommitMs, std::memory_order_relaxed);
    auto maximum = latencyMaxMs_.load(std::memory_order_relaxed);
    while (maximum < queueToCommitMs &&
           !latencyMaxMs_.compare_exchange_weak(maximum, queueToCommitMs,
                                                std::memory_order_relaxed))
    {
    }
}

void TelemetryMetrics::setBrokerConnected(bool connected) noexcept
{
    const bool previous = brokerConnected_.exchange(connected, std::memory_order_relaxed);
    if (connected && !previous && brokerConnectedOnce_.exchange(true, std::memory_order_relaxed))
        ++brokerReconnects_;
}

MetricsSnapshot TelemetryMetrics::snapshot() const noexcept
{
    return {received_.load(std::memory_order_relaxed),
            persisted_.load(std::memory_order_relaxed),
            duplicates_.load(std::memory_order_relaxed),
            rejected_.load(std::memory_order_relaxed),
            queueFull_.load(std::memory_order_relaxed),
            retries_.load(std::memory_order_relaxed),
            latencyCount_.load(std::memory_order_relaxed),
            latencySumMs_.load(std::memory_order_relaxed),
            latencyMaxMs_.load(std::memory_order_relaxed),
            brokerReconnects_.load(std::memory_order_relaxed),
            brokerConnected_.load(std::memory_order_relaxed)};
}

TelemetryMetrics &telemetryMetrics()
{
    static TelemetryMetrics metrics;
    return metrics;
}

}  // namespace labscope::metrics
