#include "telemetry/TelemetryValidator.h"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
constexpr int64_t kNowMs = 1791710400000LL;
const std::string kTopic =
    "labscope/v1/sites/plant-a/devices/pump-07/telemetry";

std::string payload(const Json::Value &value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value validPayload()
{
    Json::Value root;
    root["schema_version"] = 1;
    root["seq"] = Json::Int64(12345);
    root["timestamp_ms"] = Json::Int64(kNowMs);
    root["metrics"]["temperature_c"] = 38.4;
    root["metrics"]["vibration_mm_s"] = 2.1;
    return root;
}

bool expectCode(const std::string &name,
                const labscope::telemetry::ValidationResult &result,
                labscope::telemetry::ValidationCode expected)
{
    if (result.code == expected)
        return true;
    std::cerr << name << ": expected code " << static_cast<int>(expected)
              << ", got " << static_cast<int>(result.code) << " ("
              << result.reason << ")\n";
    return false;
}
}  // namespace

int main()
{
    using labscope::telemetry::ValidationCode;
    using labscope::telemetry::ValidationOptions;
    using labscope::telemetry::validateTelemetry;

    int failures = 0;
    auto check = [&failures](bool passed) {
        if (!passed)
            ++failures;
    };

    auto valid = validateTelemetry(kTopic, payload(validPayload()), kNowMs);
    check(expectCode("valid envelope", valid, ValidationCode::Valid));
    check(valid.envelope.has_value());
    if (valid.envelope)
    {
        check(valid.envelope->siteId == "plant-a");
        check(valid.envelope->deviceId == "pump-07");
        check(valid.envelope->seq == 12345);
        check(valid.envelope->receivedTimeMs == kNowMs);
        check(valid.envelope->metrics["temperature_c"].asDouble() == 38.4);
    }

    check(expectCode("wrong topic", validateTelemetry("bad/topic", payload(validPayload()), kNowMs),
                     ValidationCode::InvalidTopic));
    check(expectCode("unsafe topic id",
                     validateTelemetry("labscope/v1/sites/../devices/pump/telemetry",
                                       payload(validPayload()), kNowMs),
                     ValidationCode::InvalidTopic));

    auto missingSchema = validPayload();
    missingSchema.removeMember("schema_version");
    check(expectCode("missing schema", validateTelemetry(kTopic, payload(missingSchema), kNowMs),
                     ValidationCode::UnsupportedSchema));

    auto badSequence = validPayload();
    badSequence["seq"] = -1;
    check(expectCode("negative sequence", validateTelemetry(kTopic, payload(badSequence), kNowMs),
                     ValidationCode::InvalidSequence));
    badSequence["seq"] = true;
    check(expectCode("boolean sequence", validateTelemetry(kTopic, payload(badSequence), kNowMs),
                     ValidationCode::InvalidSequence));
    badSequence["seq"] = 1.5;
    check(expectCode("fractional sequence", validateTelemetry(kTopic, payload(badSequence), kNowMs),
                     ValidationCode::InvalidSequence));

    auto oldTimestamp = validPayload();
    oldTimestamp["timestamp_ms"] = Json::Int64(kNowMs - 5000);
    check(expectCode("out-of-range timestamp",
                     validateTelemetry(kTopic, payload(oldTimestamp), kNowMs,
                                       ValidationOptions{1000}),
                     ValidationCode::InvalidTimestamp));

    auto emptyMetrics = validPayload();
    emptyMetrics["metrics"] = Json::objectValue;
    check(expectCode("empty metrics", validateTelemetry(kTopic, payload(emptyMetrics), kNowMs),
                     ValidationCode::InvalidMetrics));

    auto tooManyMetrics = validPayload();
    tooManyMetrics["metrics"] = Json::objectValue;
    for (int i = 0; i < 33; ++i)
        tooManyMetrics["metrics"]["m" + std::to_string(i)] = i;
    check(expectCode("too many metrics",
                     validateTelemetry(kTopic, payload(tooManyMetrics), kNowMs),
                     ValidationCode::InvalidMetrics));

    auto nonNumericMetric = validPayload();
    nonNumericMetric["metrics"]["temperature_c"] = "hot";
    check(expectCode("non-numeric metric",
                     validateTelemetry(kTopic, payload(nonNumericMetric), kNowMs),
                     ValidationCode::InvalidMetrics));

    const std::string nonFinite =
        R"({"schema_version":1,"seq":1,"timestamp_ms":1791710400000,"metrics":{"x":1e999}})";
    check(validateTelemetry(kTopic, nonFinite, kNowMs).code != ValidationCode::Valid);

    auto malformed = validateTelemetry(kTopic, "{not-json", kNowMs);
    check(expectCode("malformed JSON", malformed, ValidationCode::InvalidPayload));

    auto oversized = std::string(16 * 1024 + 1, ' ');
    check(expectCode("oversized payload", validateTelemetry(kTopic, oversized, kNowMs),
                     ValidationCode::PayloadTooLarge));

    if (failures != 0)
    {
        std::cerr << failures << " telemetry validator assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "telemetry validator cases passed\n";
    return EXIT_SUCCESS;
}
