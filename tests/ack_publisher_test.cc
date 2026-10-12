#include "../src/ingest/AckPublisher.h"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void testAckWireContract()
{
    const auto ack = labscope::ingest::serializeDeviceAck(
        {"plant-1", "pump_7", 42, labscope::ingest::AckStatus::Persisted, 1730000000123});
    expect(ack.has_value(), "valid ACK serializes");
    if (!ack)
        return;

    expect(ack->topic == "labscope/v1/sites/plant-1/devices/pump_7/acks",
           "ACK uses device-scoped topic");
    Json::CharReaderBuilder builder;
    Json::Value payload;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    expect(reader->parse(ack->payload.data(),
                         ack->payload.data() + ack->payload.size(),
                         &payload,
                         &errors),
           "ACK payload is valid JSON");
    expect(payload["schema_version"].asInt() == 1, "ACK schema version is present");
    expect(payload["seq"].asInt64() == 42, "ACK sequence is preserved");
    expect(payload["status"].asString() == "persisted", "ACK status is persisted");
    expect(payload["confirmed_ms"].asInt64() == 1730000000123,
           "ACK confirmation time is preserved");
}

void testAckRejectsUnsafeValues()
{
    expect(!labscope::ingest::serializeDeviceAck(
                {"plant/other", "pump_7", 42,
                 labscope::ingest::AckStatus::Rejected, 1730000000123}),
           "topic injection is rejected");
    expect(!labscope::ingest::serializeDeviceAck(
                {"plant-1", "pump_7", -1,
                 labscope::ingest::AckStatus::Rejected, 1730000000123}),
           "negative sequence is rejected");
    expect(!labscope::ingest::serializeDeviceAck(
                {"plant-1", "pump_7", 42,
                 labscope::ingest::AckStatus::Rejected, 0}),
           "missing confirmation time is rejected");
}
}  // namespace

int main()
{
    testAckWireContract();
    testAckRejectsUnsafeValues();
    if (failures != 0)
    {
        std::cerr << failures << " ACK publisher test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "ACK publisher tests passed\n";
    return EXIT_SUCCESS;
}
