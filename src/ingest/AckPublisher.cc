#include "AckPublisher.h"

namespace labscope::ingest
{

AckPublisher::AckPublisher() : state_(std::make_shared<State>())
{
}

void AckPublisher::setClient(MQTTAsync client)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->client = client;
}

bool AckPublisher::publish(const DeviceAck &ack) const
{
    auto wire = serializeDeviceAck(ack);
    if (!wire)
        return false;

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->client)
        return false;

    MQTTAsync_message message = MQTTAsync_message_initializer;
    message.payload = const_cast<char *>(wire->payload.data());
    message.payloadlen = static_cast<int>(wire->payload.size());
    message.qos = 1;
    message.retained = 0;
    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
    return MQTTAsync_sendMessage(state_->client,
                                 wire->topic.c_str(),
                                 &message,
                                 &response) ==
           MQTTASYNC_SUCCESS;
}

}  // namespace labscope::ingest
