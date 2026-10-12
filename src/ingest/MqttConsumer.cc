#include "MqttConsumer.h"

#include "../metrics/TelemetryMetrics.h"
#include "../telemetry/TelemetryValidator.h"

#include <MQTTProperties.h>
#include <MQTTClientPersistence.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace labscope::ingest
{
namespace
{
int64_t currentTimeMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool validId(std::string_view value)
{
    if (value.empty() || value.size() > 63)
        return false;
    auto alnum = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (!alnum(static_cast<unsigned char>(value.front())))
        return false;
    for (unsigned char ch : value)
        if (!alnum(ch) && ch != '-' && ch != '_')
            return false;
    return true;
}

bool parseRejectTarget(std::string_view topic,
                       std::string_view payload,
                       std::string &siteId,
                       std::string &deviceId,
                       int64_t &seq)
{
    std::string_view parts[7];
    size_t partCount = 0;
    size_t start = 0;
    while (start <= topic.size())
    {
        if (partCount == 7)
            return false;
        const size_t slash = topic.find('/', start);
        const size_t end = slash == std::string_view::npos ? topic.size() : slash;
        parts[partCount++] = topic.substr(start, end - start);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    if (partCount != 7 || parts[0] != "labscope" || parts[1] != "v1" ||
        parts[2] != "sites" || parts[4] != "devices" || parts[6] != "telemetry" ||
        !validId(parts[3]) || !validId(parts[5]))
        return false;

    Json::Value root;
    std::string parseError;
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(payload.data(), payload.data() + payload.size(), &root, &parseError) ||
        !root.isObject() || !root.isMember("seq") || !root["seq"].isInt64() ||
        root["seq"].asInt64() < 0)
        return false;

    siteId.assign(parts[3]);
    deviceId.assign(parts[5]);
    seq = root["seq"].asInt64();
    return true;
}
}  // namespace

MqttConsumer::MqttConsumer(MqttConsumerConfig config,
                           IngestionService &ingestionService)
    : config_(std::move(config)), ingestionService_(ingestionService)
{
}

MqttConsumer::~MqttConsumer()
{
    stop();
    if (connectProperties_.count > 0 || connectProperties_.array != nullptr)
        MQTTProperties_free(&connectProperties_);
}

void MqttConsumer::start()
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true))
        return;
    stopping_ = false;

    try
    {
        if (config_.brokerUri.empty() || config_.clientId.empty() ||
            config_.topicFilter.empty() || config_.receiveMaximum < 1 ||
            config_.receiveMaximum > 65535 || config_.sessionExpirySeconds <= 0 ||
            config_.reconnectMinSeconds < 1 ||
            config_.reconnectMaxSeconds < config_.reconnectMinSeconds)
            throw std::invalid_argument("invalid MQTT consumer configuration");

        if (!config_.username.empty() && config_.password.empty())
            throw std::invalid_argument("MQTT password is required when username is set");

        const std::filesystem::path persistencePath =
            config_.persistenceDirectory.empty()
                ? std::filesystem::path("./data/mqtt")
                : std::filesystem::path(config_.persistenceDirectory);
        std::filesystem::create_directories(persistencePath);

        MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer5;
        createOptions.sendWhileDisconnected = 1;
        createOptions.maxBufferedMessages = 512;
        createOptions.allowDisconnectedSendAtAnyTime = 1;
        createOptions.deleteOldestMessages = 0;
        createOptions.restoreMessages = 1;

        const std::string persistenceDirectory = persistencePath.string();
        const int createResult = MQTTAsync_createWithOptions(
            &client_,
            config_.brokerUri.c_str(),
            config_.clientId.c_str(),
            MQTTCLIENT_PERSISTENCE_DEFAULT,
            const_cast<char *>(persistenceDirectory.c_str()),
            &createOptions);
        if (createResult != MQTTASYNC_SUCCESS)
            throw std::runtime_error("Paho could not create the MQTT async client");

        if (MQTTAsync_setCallbacks(client_,
                                   this,
                                   &MqttConsumer::connectionLost,
                                   &MqttConsumer::messageArrived,
                                   nullptr) != MQTTASYNC_SUCCESS)
            throw std::runtime_error("Paho could not register MQTT callbacks");
        MQTTAsync_setConnected(client_, this, &MqttConsumer::connectedCallback);

        MQTTProperties_free(&connectProperties_);
        connectProperties_ = MQTTProperties_initializer;
        MQTTProperty expiry{};
        expiry.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
        expiry.value.integer4 = static_cast<unsigned int>(config_.sessionExpirySeconds);
        if (MQTTProperties_add(&connectProperties_, &expiry) != MQTTASYNC_SUCCESS)
            throw std::runtime_error("Paho could not set MQTT session expiry");
        MQTTProperty receiveMaximum{};
        receiveMaximum.identifier = MQTTPROPERTY_CODE_RECEIVE_MAXIMUM;
        receiveMaximum.value.integer2 = static_cast<unsigned short>(config_.receiveMaximum);
        if (MQTTProperties_add(&connectProperties_, &receiveMaximum) != MQTTASYNC_SUCCESS)
            throw std::runtime_error("Paho could not set the receive maximum");

        MQTTAsync_connectOptions connectOptions = MQTTAsync_connectOptions_initializer5;
        connectOptions.keepAliveInterval = 30;
        connectOptions.cleanstart = 0;
        connectOptions.automaticReconnect = 1;
        connectOptions.minRetryInterval = config_.reconnectMinSeconds;
        connectOptions.maxRetryInterval = config_.reconnectMaxSeconds;
        connectOptions.username = config_.username.empty() ? nullptr : config_.username.c_str();
        connectOptions.password = config_.password.empty() ? nullptr : config_.password.c_str();
        connectOptions.connectProperties = &connectProperties_;
        connectOptions.onSuccess = &MqttConsumer::connectSucceeded;
        connectOptions.onFailure = &MqttConsumer::connectFailed;
        connectOptions.context = this;

        ackPublisher_.setClient(client_);
        connectionAttemptPending_ = true;
        const int connectResult = MQTTAsync_connect(client_, &connectOptions);
        if (connectResult != MQTTASYNC_SUCCESS)
        {
            connectionAttemptPending_ = false;
            throw std::runtime_error("Paho rejected the MQTT connect request");
        }
    }
    catch (...)
    {
        started_ = false;
        stopping_ = true;
        ackPublisher_.setClient(nullptr);
        MQTTAsync client = nullptr;
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            client = client_;
            client_ = nullptr;
        }
        if (client)
            MQTTAsync_destroy(&client);
        throw;
    }
}

void MqttConsumer::stop()
{
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false))
        return;
    stopping_ = true;
    const bool wasConnected = connected_.exchange(false);
    metrics::telemetryMetrics().setBrokerConnected(false);

    // Disable app ACK publication before disconnecting/destroying the Paho handle.
    ackPublisher_.setClient(nullptr);
    MQTTAsync client = nullptr;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        client = client_;
        client_ = nullptr;
    }
    if (client)
    {
        {
            std::unique_lock<std::mutex> lock(clientMutex_);
            lifecycleCondition_.wait_for(lock, std::chrono::seconds(2), [this] {
                return !connectionAttemptPending_.load();
            });
        }
        if (wasConnected && !connectionAttemptPending_)
        {
            MQTTAsync_disconnectOptions disconnectOptions =
                MQTTAsync_disconnectOptions_initializer5;
            disconnectOptions.timeout = 2000;
            disconnectOptions.onSuccess = &MqttConsumer::disconnectSucceeded;
            disconnectOptions.onFailure = &MqttConsumer::disconnectFailed;
            disconnectOptions.context = this;
            {
                std::lock_guard<std::mutex> lock(clientMutex_);
                disconnectComplete_ = false;
            }
            const int disconnectResult = MQTTAsync_disconnect(client, &disconnectOptions);
            if (disconnectResult == MQTTASYNC_SUCCESS)
            {
                std::unique_lock<std::mutex> lock(clientMutex_);
                lifecycleCondition_.wait_for(lock, std::chrono::seconds(2), [this] {
                    return disconnectComplete_;
                });
            }
        }
        MQTTAsync_destroy(&client);
    }
}

bool MqttConsumer::connected() const noexcept
{
    return connected_.load();
}

uint64_t MqttConsumer::queueFullCount() const noexcept
{
    return queueFullCount_.load();
}

uint64_t MqttConsumer::invalidMessageCount() const noexcept
{
    return invalidMessageCount_.load();
}

void MqttConsumer::connectionLost(void *context, char *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self)
    {
        self->connected_ = false;
        metrics::telemetryMetrics().setBrokerConnected(false);
    }
}

void MqttConsumer::connectedCallback(void *context, char *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self && !self->stopping_)
    {
        self->connected_ = true;
        metrics::telemetryMetrics().setBrokerConnected(true);
        self->subscribe();
    }
}

int MqttConsumer::messageArrived(void *context,
                                 char *topicName,
                                 int topicLength,
                                 MQTTAsync_message *message)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (!self || !message)
    {
        if (message)
            MQTTAsync_freeMessage(&message);
        if (topicName)
            MQTTAsync_free(topicName);
        return 1;
    }
    const int result = self->receiveMessage(topicName, topicLength, message);
    if (result != 0)
    {
        MQTTAsync_freeMessage(&message);
        if (topicName)
            MQTTAsync_free(topicName);
    }
    return result;
}

void MqttConsumer::connectSucceeded(void *context, MQTTAsync_successData *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self)
    {
        self->connectionAttemptPending_ = false;
        self->lifecycleCondition_.notify_all();
    }
    // The connected callback handles the initial connection and automatic
    // reconnects. Keep subscription setup there so the first connection only
    // submits one request.
}

void MqttConsumer::connectFailed(void *context, MQTTAsync_failureData *response)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self)
    {
        self->connectionAttemptPending_ = false;
        self->lifecycleCondition_.notify_all();
    }
    if (self && !self->stopping_)
    {
        self->connected_ = false;
        metrics::telemetryMetrics().setBrokerConnected(false);
        LOG_ERROR << "MQTT connection failed: "
                  << (response ? response->message : "unknown error");
    }
}

void MqttConsumer::disconnectSucceeded(void *context, MQTTAsync_successData *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self)
    {
        {
            std::lock_guard<std::mutex> lock(self->clientMutex_);
            self->disconnectComplete_ = true;
        }
        self->lifecycleCondition_.notify_all();
    }
}

void MqttConsumer::disconnectFailed(void *context, MQTTAsync_failureData *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self)
    {
        {
            std::lock_guard<std::mutex> lock(self->clientMutex_);
            self->disconnectComplete_ = true;
        }
        self->lifecycleCondition_.notify_all();
    }
}

void MqttConsumer::subscribeSucceeded(void *context, MQTTAsync_successData5 *)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self && !self->stopping_)
        LOG_INFO << "MQTT telemetry subscription active";
}

void MqttConsumer::subscribeFailed(void *context, MQTTAsync_failureData5 *response)
{
    auto *self = static_cast<MqttConsumer *>(context);
    if (self && !self->stopping_)
        LOG_ERROR << "MQTT telemetry subscription failed: "
                  << (response ? response->code : 0);
}

void MqttConsumer::subscribe()
{
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!client_ || stopping_)
        return;
    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
    response.onSuccess5 = &MqttConsumer::subscribeSucceeded;
    response.onFailure5 = &MqttConsumer::subscribeFailed;
    response.context = this;
    const int result = MQTTAsync_subscribe(client_, config_.topicFilter.c_str(), 1, &response);
    if (result != MQTTASYNC_SUCCESS)
        LOG_ERROR << "Paho rejected MQTT telemetry subscription request: " << result;
}

int MqttConsumer::receiveMessage(const char *topicName,
                                 int topicLength,
                                 MQTTAsync_message *message)
{
    if (!topicName || !message || message->payloadlen < 0)
        return 1;
    if (stopping_)
        return 1;

    try
    {
        const std::string topic = topicLength > 0
                                      ? std::string(topicName,
                                                    static_cast<std::size_t>(topicLength))
                                      : std::string(topicName);
        const char *payloadData = static_cast<const char *>(message->payload);
        const std::string_view payload(payloadData ? payloadData : "",
                                       static_cast<std::size_t>(message->payloadlen));
        auto validation = telemetry::validateTelemetry(
            topic,
            payload,
            currentTimeMs(),
            telemetry::ValidationOptions{config_.maxTimestampSkewMs});

        if (validation.code != telemetry::ValidationCode::Valid || !validation.envelope)
        {
            ++invalidMessageCount_;
            metrics::telemetryMetrics().recordInvalid();
            std::string siteId;
            std::string deviceId;
            int64_t seq = -1;
            if (parseRejectTarget(topic, payload, siteId, deviceId, seq))
            {
                ackPublisher_.publish(
                    {std::move(siteId), std::move(deviceId), seq, AckStatus::Rejected,
                     currentTimeMs()});
            }
            return 1;
        }

        const auto siteId = validation.envelope->siteId;
        const auto deviceId = validation.envelope->deviceId;
        const auto seq = validation.envelope->seq;
        const auto receivedTimeMs = validation.envelope->receivedTimeMs;
        metrics::telemetryMetrics().recordReceived();
        CompletionCallback completion =
            [publisher = ackPublisher_, siteId, deviceId, seq, receivedTimeMs](IngestionResult result) {
                const auto finishedAt = currentTimeMs();
                const auto elapsed = finishedAt > receivedTimeMs
                                         ? static_cast<uint64_t>(finishedAt - receivedTimeMs)
                                         : 0;
                const auto outcome = result.status == IngestionStatus::Persisted
                                         ? metrics::IngestOutcome::Persisted
                                     : result.status == IngestionStatus::Duplicate
                                         ? metrics::IngestOutcome::Duplicate
                                     : result.status == IngestionStatus::Rejected
                                         ? metrics::IngestOutcome::Rejected
                                         : metrics::IngestOutcome::Retryable;
                metrics::telemetryMetrics().recordResult(outcome, elapsed);
                if (result.status == IngestionStatus::RetryableError)
                {
                    LOG_ERROR << "Telemetry persistence will be retried: " << result.reason;
                    return;  // Device outbox retains and retries this sequence.
                }
                AckStatus status = AckStatus::Rejected;
                if (result.status == IngestionStatus::Persisted)
                    status = AckStatus::Persisted;
                else if (result.status == IngestionStatus::Duplicate)
                    status = AckStatus::Duplicate;
                publisher.publish({siteId, deviceId, seq, status, currentTimeMs()});
            };

        auto envelope = std::move(*validation.envelope);
        const QueueResult queued =
            ingestionService_.submit(std::move(envelope), std::move(completion));
        if (queued == QueueResult::Full)
        {
            ++queueFullCount_;
            metrics::telemetryMetrics().recordQueueFull();
            // The transport delivery is released, but no application ACK is sent.
            // The device's durable outbox republishes the same sequence after timeout.
        }
        return 1;
    }
    catch (const std::exception &error)
    {
        LOG_ERROR << "MQTT telemetry callback failed: " << error.what();
        ++invalidMessageCount_;
        metrics::telemetryMetrics().recordInvalid();
        return 1;
    }
    catch (...)
    {
        ++invalidMessageCount_;
        metrics::telemetryMetrics().recordInvalid();
        return 1;
    }
}

}  // namespace labscope::ingest
