#include "AckPublisher.h"

#include <json/json.h>

namespace labscope::ingest
{
namespace
{
bool validTopicSegment(const std::string &value)
{
    const auto alnum = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (value.empty() || value.size() > 63 ||
        !alnum(static_cast<unsigned char>(value.front())))
        return false;
    for (unsigned char ch : value)
    {
        if (!alnum(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}
}  // namespace

const char *toString(AckStatus status) noexcept
{
    switch (status)
    {
        case AckStatus::Persisted:
            return "persisted";
        case AckStatus::Duplicate:
            return "duplicate";
        case AckStatus::Rejected:
            return "rejected";
    }
    return "rejected";
}

std::optional<AckWireMessage> serializeDeviceAck(const DeviceAck &ack)
{
    if (!validTopicSegment(ack.siteId) || !validTopicSegment(ack.deviceId) ||
        ack.seq < 0 || ack.confirmedMs <= 0)
        return std::nullopt;

    Json::Value payload;
    payload["schema_version"] = 1;
    payload["seq"] = Json::Int64(ack.seq);
    payload["status"] = toString(ack.status);
    payload["confirmed_ms"] = Json::Int64(ack.confirmedMs);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    AckWireMessage wire;
    wire.topic = "labscope/v1/sites/" + ack.siteId + "/devices/" + ack.deviceId +
                 "/acks";
    wire.payload = Json::writeString(writer, payload);
    return wire;
}

}  // namespace labscope::ingest
