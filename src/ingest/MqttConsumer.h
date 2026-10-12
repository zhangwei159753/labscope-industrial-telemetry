#pragma once

#include "AckPublisher.h"
#include "IngestionService.h"
#include "TelemetrySource.h"

#include <MQTTAsync.h>

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace labscope::ingest
{

struct MqttConsumerConfig
{
    std::string brokerUri{"tcp://127.0.0.1:1883"};
    std::string clientId{"labscope-ingest-v1"};
    std::string username;
    std::string password;
    std::string persistenceDirectory{"./data/mqtt"};
    std::string topicFilter{"labscope/v1/sites/+/devices/+/telemetry"};
    int reconnectMinSeconds{1};
    int reconnectMaxSeconds{30};
    int sessionExpirySeconds{86400};
    int receiveMaximum{256};
    int64_t maxTimestampSkewMs{300000};
};

class MqttConsumer final : public TelemetrySource
{
  public:
    MqttConsumer(MqttConsumerConfig config, IngestionService &ingestionService);
    ~MqttConsumer() override;

    MqttConsumer(const MqttConsumer &) = delete;
    MqttConsumer &operator=(const MqttConsumer &) = delete;

    void start() override;
    void stop() override;
    bool connected() const noexcept;
    uint64_t queueFullCount() const noexcept;
    uint64_t invalidMessageCount() const noexcept;

  private:
    static void connectionLost(void *context, char *cause);
    static void connectedCallback(void *context, char *cause);
    static int messageArrived(void *context,
                              char *topicName,
                              int topicLength,
                              MQTTAsync_message *message);
    static void connectSucceeded(void *context, MQTTAsync_successData *response);
    static void connectFailed(void *context, MQTTAsync_failureData *response);
    static void subscribeSucceeded(void *context, MQTTAsync_successData5 *response);
    static void subscribeFailed(void *context, MQTTAsync_failureData5 *response);
    static void disconnectSucceeded(void *context, MQTTAsync_successData *response);
    static void disconnectFailed(void *context, MQTTAsync_failureData *response);

    void subscribe();
    int receiveMessage(const char *topicName,
                       int topicLength,
                       MQTTAsync_message *message);
    MqttConsumerConfig config_;
    IngestionService &ingestionService_;
    MQTTAsync client_{nullptr};
    mutable std::mutex clientMutex_;
    std::condition_variable lifecycleCondition_;
    AckPublisher ackPublisher_;
    MQTTProperties connectProperties_ = MQTTProperties_initializer;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> connectionAttemptPending_{false};
    bool disconnectComplete_{false};
    std::atomic<uint64_t> queueFullCount_{0};
    std::atomic<uint64_t> invalidMessageCount_{0};
};

}  // namespace labscope::ingest
