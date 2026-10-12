#pragma once

#include "../telemetry/TelemetryEnvelope.h"

#include <cstdint>
#include <functional>
#include <string>

namespace labscope::ingest
{

enum class IngestionStatus
{
    Persisted,
    Duplicate,
    Rejected,
    RetryableError
};

struct IngestionResult
{
    IngestionStatus status{IngestionStatus::RetryableError};
    std::string reason;
};

using CompletionCallback = std::function<void(IngestionResult)>;

struct QueuedTelemetry
{
    telemetry::TelemetryEnvelope envelope;
    CompletionCallback completion;
};

enum class QueueResult
{
    Accepted,
    Full,
    Stopping
};

}  // namespace labscope::ingest
