#include "../src/ingest/IngestionService.h"
#include "../src/ingest/MqttConsumer.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

bool rejectsZeroSessionExpiry(labscope::ingest::IngestionService &ingestion)
{
    labscope::ingest::MqttConsumerConfig config;
    config.sessionExpirySeconds = 0;
    labscope::ingest::MqttConsumer consumer(std::move(config), ingestion);

    try
    {
        consumer.start();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    catch (...)
    {
        std::cerr << "unexpected exception for zero session expiry\n";
        return false;
    }

    std::cerr << "zero MQTT session expiry was accepted\n";
    return false;
}

bool stopsWhileConnectIsPending(labscope::ingest::IngestionService &ingestion)
{
    labscope::ingest::MqttConsumerConfig config;
    config.brokerUri = "tcp://127.0.0.1:1";
    config.clientId = "labscope-test-stop-pending";
    config.persistenceDirectory = "./build/test_mqtt_persistence";
    labscope::ingest::MqttConsumer consumer(std::move(config), ingestion);
    try
    {
        consumer.start();
        consumer.stop();
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "start/stop with unavailable broker failed: " << error.what() << '\n';
        return false;
    }
}

int main()
{
    labscope::ingest::IngestionService ingestion(
        1,
        1,
        1,
        [](std::vector<labscope::ingest::QueuedTelemetry>,
           labscope::ingest::BatchCompletionCallback) {});
    if (!rejectsZeroSessionExpiry(ingestion) ||
        !stopsWhileConnectIsPending(ingestion))
        return EXIT_FAILURE;
    std::cout << "MQTT config and lifecycle tests passed\n";
    return EXIT_SUCCESS;
}
