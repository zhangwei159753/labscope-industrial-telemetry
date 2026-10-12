#include "TelemetryValidator.h"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace labscope::telemetry
{
namespace
{
constexpr size_t kMaxPayloadBytes = 16 * 1024;
constexpr size_t kMaxMetrics = 32;
constexpr size_t kMaxMetricNameBytes = 128;

ValidationResult invalid(ValidationCode code, std::string reason)
{
    return {code, std::nullopt, std::move(reason)};
}

bool validIdentifier(std::string_view value)
{
    if (value.empty() || value.size() > 63)
        return false;
    auto isAlphaNumeric = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (!isAlphaNumeric(static_cast<unsigned char>(value.front())))
        return false;
    for (unsigned char ch : value)
    {
        if (!isAlphaNumeric(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

bool validMetricName(std::string_view value)
{
    if (value.empty() || value.size() > kMaxMetricNameBytes)
        return false;
    for (unsigned char ch : value)
    {
        const bool alphaNumeric = (ch >= 'a' && ch <= 'z') ||
                                  (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9');
        if (!alphaNumeric && ch != '_' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

bool parseTopic(std::string_view topic, std::string &siteId, std::string &deviceId)
{
    constexpr std::string_view expected[] = {
        "labscope", "v1", "sites", {}, "devices", {}, "telemetry"};
    std::string_view segments[7];
    size_t segmentCount = 0;
    size_t start = 0;
    while (start <= topic.size())
    {
        if (segmentCount == 7)
            return false;
        const size_t slash = topic.find('/', start);
        const size_t end = slash == std::string_view::npos ? topic.size() : slash;
        segments[segmentCount++] = topic.substr(start, end - start);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    if (segmentCount != 7)
        return false;
    for (size_t index : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{6}})
    {
        if (segments[index] != expected[index])
            return false;
    }
    if (!validIdentifier(segments[3]) || !validIdentifier(segments[5]))
        return false;
    siteId.assign(segments[3]);
    deviceId.assign(segments[5]);
    return true;
}

bool parseJson(std::string_view payload, Json::Value &root, std::string &error)
{
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["allowSpecialFloats"] = false;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(payload.data(), payload.data() + payload.size(), &root, &error);
}
}  // namespace

ValidationResult validateTelemetry(std::string_view topic,
                                   std::string_view payload,
                                   int64_t nowMs,
                                   ValidationOptions options)
{
    if (payload.size() > kMaxPayloadBytes)
        return invalid(ValidationCode::PayloadTooLarge,
                       "payload exceeds 16384 bytes");

    std::string siteId;
    std::string deviceId;
    if (!parseTopic(topic, siteId, deviceId))
        return invalid(ValidationCode::InvalidTopic,
                       "topic must identify one site and device telemetry stream");

    Json::Value root;
    std::string parseError;
    if (!parseJson(payload, root, parseError) || !root.isObject())
        return invalid(ValidationCode::InvalidPayload,
                       parseError.empty() ? "payload must be a JSON object" : parseError);

    if (!root.isMember("schema_version") || !root["schema_version"].isInt() ||
        root["schema_version"].asInt() != 1)
        return invalid(ValidationCode::UnsupportedSchema,
                       "schema_version must be the integer 1");

    if (!root.isMember("seq") || !root["seq"].isInt64() ||
        root["seq"].asInt64() < 0)
        return invalid(ValidationCode::InvalidSequence,
                       "seq must be a non-negative signed 64-bit integer");

    if (!root.isMember("timestamp_ms") || !root["timestamp_ms"].isInt64() ||
        root["timestamp_ms"].asInt64() <= 0 || nowMs <= 0 ||
        options.maxTimestampSkewMs < 0)
        return invalid(ValidationCode::InvalidTimestamp,
                       "timestamp_ms and server time must be positive integers");

    const int64_t deviceTimeMs = root["timestamp_ms"].asInt64();
    const int64_t skewMs = deviceTimeMs >= nowMs ? deviceTimeMs - nowMs
                                                  : nowMs - deviceTimeMs;
    if (skewMs > options.maxTimestampSkewMs)
        return invalid(ValidationCode::InvalidTimestamp,
                       "timestamp_ms is outside the configured clock-skew window");

    const Json::Value &metrics = root["metrics"];
    if (!metrics.isObject() || metrics.empty() || metrics.size() > kMaxMetrics)
        return invalid(ValidationCode::InvalidMetrics,
                       "metrics must contain between 1 and 32 named values");
    for (const auto &name : metrics.getMemberNames())
    {
        const auto &value = metrics[name];
        if (!validMetricName(name) || !value.isNumeric() ||
            !std::isfinite(value.asDouble()))
            return invalid(ValidationCode::InvalidMetrics,
                           "metrics must use safe names and finite numeric values");
    }

    TelemetryEnvelope envelope;
    envelope.siteId = std::move(siteId);
    envelope.deviceId = std::move(deviceId);
    envelope.seq = root["seq"].asInt64();
    envelope.deviceTimeMs = deviceTimeMs;
    envelope.receivedTimeMs = nowMs;
    envelope.metrics = metrics;
    return {ValidationCode::Valid, std::move(envelope), {}};
}

}  // namespace labscope::telemetry
