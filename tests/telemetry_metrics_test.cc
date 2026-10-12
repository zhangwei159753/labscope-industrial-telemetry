#include "metrics/TelemetryMetrics.h"

#include <cassert>

int main()
{
    labscope::metrics::TelemetryMetrics metrics;
    metrics.recordReceived();
    metrics.recordInvalid();
    metrics.recordQueueFull();
    metrics.recordResult(labscope::metrics::IngestOutcome::Persisted, 12);
    metrics.recordResult(labscope::metrics::IngestOutcome::Duplicate, 7);
    metrics.recordResult(labscope::metrics::IngestOutcome::Retryable, 31);

    const auto snapshot = metrics.snapshot();
    assert(snapshot.received == 1);
    assert(snapshot.persisted == 1);
    assert(snapshot.duplicates == 1);
    assert(snapshot.rejected == 1);
    assert(snapshot.queueFull == 1);
    assert(snapshot.retries == 1);
    assert(snapshot.commitLatencyCount == 2);
    assert(snapshot.commitLatencySumMs == 19);
    assert(snapshot.commitLatencyMaxMs == 12);

    metrics.setBrokerConnected(true);
    metrics.setBrokerConnected(true);
    metrics.setBrokerConnected(false);
    metrics.setBrokerConnected(true);
    assert(metrics.snapshot().brokerReconnects == 1);
    return 0;
}
