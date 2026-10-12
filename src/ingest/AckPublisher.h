#pragma once

#include <MQTTAsync.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace labscope::ingest
{

enum class AckStatus
{
    Persisted,
    Duplicate,
    Rejected
};

struct DeviceAck
{
    std::string siteId;
    std::string deviceId;
    int64_t seq{0};
    AckStatus status{AckStatus::Rejected};
    int64_t confirmedMs{0};
};

struct AckWireMessage
{
    std::string topic;
    std::string payload;
};

std::optional<AckWireMessage> serializeDeviceAck(const DeviceAck &ack);

class AckPublisher final
{
  public:
    AckPublisher();
    void setClient(MQTTAsync client);

    // Publishes QoS 1, non-retained application ACKs; returns false if the
    // client is stopping or Paho rejects the local publish request.
    bool publish(const DeviceAck &ack) const;

  private:
    struct State
    {
        mutable std::mutex mutex;
        MQTTAsync client{nullptr};
    };
    std::shared_ptr<State> state_;
};

const char *toString(AckStatus status) noexcept;

}  // namespace labscope::ingest
