#pragma once

#include "TelemetryEnvelope.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace labscope::telemetry
{

enum class ValidationCode
{
    Valid,
    InvalidTopic,
    InvalidPayload,
    UnsupportedSchema,
    InvalidSequence,
    InvalidTimestamp,
    InvalidMetrics,
    PayloadTooLarge
};

struct ValidationOptions
{
    int64_t maxTimestampSkewMs{300000};
};

struct ValidationResult
{
    ValidationCode code{ValidationCode::InvalidPayload};
    std::optional<TelemetryEnvelope> envelope;
    std::string reason;
};

ValidationResult validateTelemetry(
    std::string_view topic,
    std::string_view payload,
    int64_t nowMs,
    ValidationOptions options = {});

}  // namespace labscope::telemetry
