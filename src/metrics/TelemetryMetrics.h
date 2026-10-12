#pragma once

#include <atomic>
#include <cstdint>

namespace labscope::metrics
{

enum class IngestOutcome { Persisted, Duplicate, Rejected, Retryable };

struct MetricsSnapshot
{
    uint64_t received{0};
    uint64_t persisted{0};
    uint64_t duplicates{0};
    uint64_t rejected{0};
    uint64_t queueFull{0};
    uint64_t retries{0};
    uint64_t commitLatencyCount{0};
    uint64_t commitLatencySumMs{0};
    uint64_t commitLatencyMaxMs{0};
    uint64_t brokerReconnects{0};
    bool brokerConnected{false};
};

class TelemetryMetrics final
{
  public:
    void recordReceived() noexcept;
    void recordInvalid() noexcept;
    void recordQueueFull() noexcept;
    void recordResult(IngestOutcome outcome, uint64_t queueToCommitMs) noexcept;
    void setBrokerConnected(bool connected) noexcept;
    MetricsSnapshot snapshot() const noexcept;

  private:
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> persisted_{0};
    std::atomic<uint64_t> duplicates_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> queueFull_{0};
    std::atomic<uint64_t> retries_{0};
    std::atomic<uint64_t> latencyCount_{0};
    std::atomic<uint64_t> latencySumMs_{0};
    std::atomic<uint64_t> latencyMaxMs_{0};
    std::atomic<uint64_t> brokerReconnects_{0};
    std::atomic<bool> brokerConnected_{false};
    std::atomic<bool> brokerConnectedOnce_{false};
};

TelemetryMetrics &telemetryMetrics();

}  // namespace labscope::metrics
