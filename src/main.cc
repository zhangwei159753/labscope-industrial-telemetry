#include <drogon/drogon.h>
#include <drogon/WebSocketController.h>

#include "telemetry_hub.h"
#include "ingest/IngestionService.h"
#include "ingest/MqttConsumer.h"
#include "storage/PartitionMaintenance.h"
#include "storage/TelemetryRepository.h"
#include "storage/TelemetryWriter.h"
#include "controllers/DeviceController.h"
#include "controllers/TelemetryController.h"
#include "controllers/AuthController.h"
#include "controllers/SiteController.h"
#include "controllers/LiveMonitorSocket.h"
#include "controllers/AlarmController.h"
#include "controllers/MetricsController.h"
#include "alarms/AlarmProcessor.h"
#include "auth/PasswordHasher.h"
#include "auth/SessionStore.h"
#include "metrics/TelemetryMetrics.h"

#include <libpq-fe.h>
#include <sodium.h>

#include <cctype>
#include <memory>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace drogon;

namespace
{
std::atomic<bool> databaseBackedMode{false};

std::optional<int64_t> parseBoundedInt(const std::string &text,
                                       int64_t minimum,
                                       int64_t maximum);

std::string envOr(const char *name, std::string fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : std::move(fallback);
}

std::string quoteConnectionValue(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('\'');
    for (char ch : value)
    {
        if (ch == '\\' || ch == '\'')
            escaped.push_back('\\');
        escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
}

bool validCliUsername(const std::string &username)
{
    if (username.size() < 3 || username.size() > 64 ||
        !std::isalnum(static_cast<unsigned char>(username.front())))
        return false;
    for (unsigned char ch : username)
        if (!std::isalnum(ch) && ch != '_' && ch != '.' && ch != '@' && ch != '-')
            return false;
    return true;
}

int createWebUserFromStdin(const std::string &username,
                           const std::string &role,
                           const std::vector<std::string> &siteIds)
{
    if (!validCliUsername(username) ||
        (role != "admin" && role != "operator" && role != "viewer") ||
        (role != "admin" && siteIds.empty()))
    {
        std::cerr << "invalid username, role, or missing site grants\n";
        return 2;
    }
    for (const auto &siteId : siteIds)
    {
        if (siteId.empty() || siteId.size() > 63 ||
            !std::isalnum(static_cast<unsigned char>(siteId.front())))
        {
            std::cerr << "invalid site id\n";
            return 2;
        }
        for (unsigned char ch : siteId)
            if (!std::isalnum(ch) && ch != '_' && ch != '-')
            {
                std::cerr << "invalid site id\n";
                return 2;
            }
    }
    const auto dbHost = envOr("LABSCOPE_DB_HOST", "127.0.0.1");
    const auto dbPort = envOr("LABSCOPE_DB_PORT", "5432");
    const auto dbName = envOr("LABSCOPE_DB_NAME", "labscope");
    const auto dbUser = envOr("LABSCOPE_DB_USER", "labscope");
    const auto dbPassword = envOr("LABSCOPE_DB_PASSWORD", "");
    const auto connectionInfo =
        "host=" + quoteConnectionValue(dbHost) +
        " port=" + quoteConnectionValue(dbPort) +
        " dbname=" + quoteConnectionValue(dbName) +
        " user=" + quoteConnectionValue(dbUser) +
        " password=" + quoteConnectionValue(dbPassword) +
        " connect_timeout='5'";
    PGconn *connection = PQconnectdb(connectionInfo.c_str());
    if (!connection || PQstatus(connection) != CONNECTION_OK)
    {
        std::cerr << "database connection failed\n";
        if (connection) PQfinish(connection);
        return 2;
    }

    std::string password;
    std::string confirmation;
    std::getline(std::cin, password);
    std::getline(std::cin, confirmation);
    const char *environment = std::getenv("LABSCOPE_ENV");
    const std::string_view environmentName = environment ? environment : "";
    const auto minimumPasswordLength =
        labscope::auth::PasswordHasher::minimumPasswordLengthForEnvironment(
            environmentName);
    if (password != confirmation || password.size() < minimumPasswordLength ||
        password.size() > 1024)
    {
        sodium_memzero(password.data(), password.size());
        sodium_memzero(confirmation.data(), confirmation.size());
        std::cerr << "password confirmation failed or password length is invalid\n";
        PQfinish(connection);
        return 2;
    }

    std::string passwordHash;
    try
    {
        passwordHash = labscope::auth::PasswordHasher::hashPassword(password);
    }
    catch (const std::exception &error)
    {
        sodium_memzero(password.data(), password.size());
        sodium_memzero(confirmation.data(), confirmation.size());
        std::cerr << error.what() << '\n';
        PQfinish(connection);
        return 2;
    }
    sodium_memzero(password.data(), password.size());
    sodium_memzero(confirmation.data(), confirmation.size());

    PGresult *beginResult = PQexec(connection, "BEGIN");
    const bool began = beginResult && PQresultStatus(beginResult) == PGRES_COMMAND_OK;
    if (beginResult) PQclear(beginResult);
    if (!began)
    {
        PQfinish(connection);
        return 2;
    }
    const char *values[] = {username.c_str(), passwordHash.c_str(), role.c_str()};
    PGresult *result = PQexecParams(
        connection,
        "INSERT INTO web_users (username, password_hash, role) VALUES ($1, $2, $3) "
        "RETURNING user_id",
        3, nullptr, values, nullptr, nullptr, 0);
    const bool inserted = result && PQresultStatus(result) == PGRES_TUPLES_OK &&
                          PQntuples(result) == 1;
    std::string userId;
    if (inserted) userId = PQgetvalue(result, 0, 0);
    if (result) PQclear(result);
    bool succeeded = inserted;
    if (succeeded)
    {
        const char *auditValues[] = {userId.c_str(), role.c_str()};
        result = PQexecParams(connection,
            "INSERT INTO audit_log (user_id, action, entity_type, entity_id, metadata) "
            "VALUES (NULL, 'web_user.create', 'web_user', $1, "
            "jsonb_build_object('role', $2::text, 'provisioned_by', 'local_cli'))",
            2, nullptr, auditValues, nullptr, nullptr, 0);
        succeeded = result && PQresultStatus(result) == PGRES_COMMAND_OK;
        if (result) PQclear(result);
    }
    for (const auto &siteId : siteIds)
    {
        if (!succeeded) break;
        const char *grantValues[] = {userId.c_str(), siteId.c_str()};
        result = PQexecParams(connection,
            "INSERT INTO web_user_sites (user_id, site_id) "
            "SELECT $1::bigint, site_id FROM sites WHERE site_id = $2 AND enabled "
            "RETURNING site_id",
            2, nullptr, grantValues, nullptr, nullptr, 0);
        const bool granted = result && PQresultStatus(result) == PGRES_TUPLES_OK &&
                             PQntuples(result) == 1;
        if (result) PQclear(result);
        if (!granted)
        {
            succeeded = false;
            break;
        }
        const char *auditValues[] = {userId.c_str(), siteId.c_str()};
        result = PQexecParams(connection,
            "INSERT INTO audit_log (user_id, action, entity_type, entity_id, site_id, metadata) "
            "VALUES (NULL, 'web_user_site.grant', 'web_user', $1, $2, "
            "jsonb_build_object('provisioned_by', 'local_cli'))",
            2, nullptr, auditValues, nullptr, nullptr, 0);
        succeeded = result && PQresultStatus(result) == PGRES_COMMAND_OK;
        if (result) PQclear(result);
    }
    result = PQexec(connection, succeeded ? "COMMIT" : "ROLLBACK");
    succeeded = succeeded && result && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (result) PQclear(result);
    PQfinish(connection);
    if (!succeeded)
    {
        std::cerr << "user was not created; check username, role, enabled sites, or existing account\n";
        return 2;
    }
    std::cout << "web user created with role " << role << '\n';
    return 0;
}

struct TelemetryRuntime
{
    drogon::orm::DbClientPtr dbClient;
    std::shared_ptr<labscope::ingest::IngestionService> ingestion;
    std::unique_ptr<labscope::ingest::MqttConsumer> mqtt;
    std::shared_ptr<labscope::alarms::AlarmProcessor> alarms;
    std::mutex maintenanceMutex;
    std::condition_variable maintenanceCondition;
    bool stopMaintenance{false};
    std::thread maintenanceThread;

    void stop()
    {
        if (alarms)
            alarms->stop();
        if (mqtt)
            mqtt->stop();
        if (ingestion)
            ingestion->stop();
        {
            std::lock_guard<std::mutex> lock(maintenanceMutex);
            stopMaintenance = true;
        }
        maintenanceCondition.notify_one();
        if (maintenanceThread.joinable())
            maintenanceThread.join();
    }
};

std::unique_ptr<TelemetryRuntime> startTelemetryRuntime()
{
    // Local WebSocket development remains available without infrastructure.
    // Compose and deployed environments enable the durable MQTT path by setting DB host.
    const char *dbHost = std::getenv("LABSCOPE_DB_HOST");
    if (!dbHost || !*dbHost)
        return {};

    const auto dbPort = envOr("LABSCOPE_DB_PORT", "5432");
    const auto dbName = envOr("LABSCOPE_DB_NAME", "labscope");
    const auto dbUser = envOr("LABSCOPE_DB_USER", "labscope");
    const auto dbPassword = envOr("LABSCOPE_DB_PASSWORD", "");
    const std::string connectionInfo =
        "host=" + quoteConnectionValue(dbHost) +
        " port=" + quoteConnectionValue(dbPort) +
        " dbname=" + quoteConnectionValue(dbName) +
        " user=" + quoteConnectionValue(dbUser) +
        " password=" + quoteConnectionValue(dbPassword) +
        " connect_timeout='5'";

    auto dbClient = drogon::orm::DbClient::newPgClient(connectionInfo, 4, true);
    labscope::storage::PartitionMaintenance maintenance(dbClient);
    struct StartupState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed{false};
        bool ready{false};
        std::string error;
    };
    auto startup = std::make_shared<StartupState>();
    maintenance.ensureStartupPartitions([startup](bool succeeded, std::string error) {
        {
            std::lock_guard<std::mutex> lock(startup->mutex);
            startup->completed = true;
            startup->ready = succeeded;
            startup->error = std::move(error);
        }
        startup->condition.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(startup->mutex);
        if (!startup->condition.wait_for(lock, std::chrono::seconds(15), [&] {
                return startup->completed;
            }))
            throw std::runtime_error("timed out creating telemetry partitions");
    }
    if (!startup->ready)
        throw std::runtime_error("database startup check failed: " + startup->error);

    const auto repository =
        std::make_shared<labscope::storage::TelemetryRepository>(dbClient);
    const auto writer = std::make_shared<labscope::storage::TelemetryWriter>(
        repository,
        [](const labscope::telemetry::TelemetryEnvelope &envelope) {
            telemetryHub().publishCommitted(envelope.siteId,
                                            envelope.deviceId,
                                            envelope.seq,
                                            envelope.deviceTimeMs,
                                            envelope.receivedTimeMs,
                                            envelope.metrics);
        });
    const auto queueCapacity = parseBoundedInt(
        envOr("LABSCOPE_INGEST_QUEUE_CAPACITY", "8192"), 128, 1000000);
    const auto workerCount = parseBoundedInt(
        envOr("LABSCOPE_INGEST_WORKERS", "4"), 1, 64);
    const auto batchSize = parseBoundedInt(
        envOr("LABSCOPE_INGEST_BATCH_SIZE", "128"), 1, 4096);
    if (!queueCapacity || !workerCount || !batchSize)
        throw std::invalid_argument("invalid ingestion queue, worker, or batch configuration");

    auto runtime = std::make_unique<TelemetryRuntime>();
    runtime->dbClient = dbClient;
    runtime->alarms = std::make_shared<labscope::alarms::AlarmProcessor>(dbClient);
    runtime->alarms->start();
    runtime->ingestion = std::make_shared<labscope::ingest::IngestionService>(
        static_cast<std::size_t>(*queueCapacity),
        static_cast<std::size_t>(*workerCount),
        static_cast<std::size_t>(*batchSize),
        [writer](std::vector<labscope::ingest::QueuedTelemetry> batch,
                 labscope::ingest::BatchCompletionCallback completion) {
            writer->persist(std::move(batch), std::move(completion));
        });
    runtime->ingestion->start();

    labscope::ingest::MqttConsumerConfig mqttConfig;
    mqttConfig.brokerUri = envOr("LABSCOPE_MQTT_URI", mqttConfig.brokerUri);
    mqttConfig.clientId = envOr("LABSCOPE_MQTT_CLIENT_ID", mqttConfig.clientId);
    mqttConfig.username = envOr("LABSCOPE_MQTT_USERNAME", "");
    mqttConfig.password = envOr("LABSCOPE_MQTT_PASSWORD", "");
    mqttConfig.persistenceDirectory = envOr("LABSCOPE_MQTT_PERSISTENCE", "./data/mqtt");
    runtime->mqtt = std::make_unique<labscope::ingest::MqttConsumer>(
        std::move(mqttConfig), *runtime->ingestion);
    runtime->mqtt->start();
    auto maintenanceService =
        std::make_shared<labscope::storage::PartitionMaintenance>(dbClient);
    runtime->maintenanceThread = std::thread([runtimePtr = runtime.get(), maintenanceService] {
        std::unique_lock<std::mutex> lock(runtimePtr->maintenanceMutex);
        while (!runtimePtr->maintenanceCondition.wait_for(
            lock, std::chrono::hours(1), [runtimePtr] { return runtimePtr->stopMaintenance; }))
        {
            lock.unlock();
            maintenanceService->runRetentionMaintenance([](bool succeeded, std::string error) {
                if (!succeeded)
                    LOG_ERROR << "telemetry partition/retention maintenance failed: " << error;
            });
            lock.lock();
        }
    });
    return runtime;
}

std::optional<int64_t> parseBoundedInt(const std::string &text,
                                       int64_t minimum,
                                       int64_t maximum)
{
    if (text.empty())
        return std::nullopt;
    int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < minimum || value > maximum)
        return std::nullopt;
    return value;
}
}  // namespace

class IngestSocket : public WebSocketController<IngestSocket>
{
  public:
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/ingest", Get);
    WS_PATH_LIST_END

    void handleNewConnection(const HttpRequestPtr &request,
                             const WebSocketConnectionPtr &connection) override
    {
        const auto deviceId = request->getParameter("device_id");
        if (!TelemetryHub::validDeviceId(deviceId))
        {
            connection->shutdown(CloseCode::kViolation, "invalid device_id");
            return;
        }
        if (databaseBackedMode)
        {
            connection->shutdown(CloseCode::kViolation,
                                 "use the configured MQTT telemetry endpoint");
            return;
        }
        connection->setContext(std::make_shared<std::string>(deviceId));
        Json::Value ready;
        ready["type"] = "ready";
        ready["device_id"] = deviceId;
        connection->sendJson(ready);
    }

    void handleNewMessage(const WebSocketConnectionPtr &connection,
                          std::string &&message,
                          const WebSocketMessageType &type) override
    {
        if (type != WebSocketMessageType::Text)
            return;
        auto deviceId = connection->getContext<std::string>();
        if (!deviceId)
            return;

        IngestResult result;
        if (message.size() > 16 * 1024)
            result = telemetryHub().reject("message exceeds 16 KiB");
        else
        {
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value sample;
            std::string errors;
            if (!reader->parse(message.data(),
                               message.data() + message.size(),
                               &sample,
                               &errors))
                result = telemetryHub().reject("invalid JSON");
            else
                result = telemetryHub().ingest(*deviceId, sample);
        }
        if (!result.accepted)
        {
            Json::Value reply;
            reply["type"] = "rejected";
            reply["reason"] = result.reason;
            connection->sendJson(reply);
        }
    }

    void handleConnectionClosed(const WebSocketConnectionPtr &) override
    {
    }
};

int main(int argc, char **argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--create-admin")
    {
        if (argc != 3)
        {
            std::cerr << "usage: lab_telemetry --create-admin USERNAME\n";
            return 2;
        }
        return createWebUserFromStdin(argv[2], "admin", {});
    }
    if (argc >= 2 && std::string(argv[1]) == "--create-user")
    {
        if (argc < 4)
        {
            std::cerr << "usage: lab_telemetry --create-user USERNAME ROLE [SITE_ID ...]\n";
            return 2;
        }
        std::vector<std::string> siteIds;
        for (int index = 4; index < argc; ++index)
            siteIds.emplace_back(argv[index]);
        return createWebUserFromStdin(argv[2], argv[3], siteIds);
    }
    const auto port = argc > 1 ? static_cast<unsigned short>(std::stoi(argv[1])) : 8848;
    const auto bindAddress = envOr("LABSCOPE_HTTP_BIND", "127.0.0.1");
    telemetryHub().start();
    auto telemetryRuntime = startTelemetryRuntime();
    databaseBackedMode = static_cast<bool>(telemetryRuntime);

    if (telemetryRuntime)
    {
        auto sessions = std::make_shared<labscope::auth::SessionStore>(
            telemetryRuntime->dbClient);
        labscope::api::AuthController::registerRoutes(
            telemetryRuntime->dbClient, sessions);
        labscope::api::SiteController::registerRoutes(telemetryRuntime->dbClient);
        labscope::api::DeviceController::registerRoutes(telemetryRuntime->dbClient);
        labscope::api::TelemetryController::registerRoutes(telemetryRuntime->dbClient);
        labscope::api::AlarmController::registerRoutes(telemetryRuntime->dbClient);
        labscope::api::MetricsController::registerRoutes(
            telemetryRuntime->dbClient, telemetryRuntime->ingestion.get(),
            telemetryRuntime->mqtt.get(), telemetryRuntime->alarms.get());
        auto liveMonitor = std::make_shared<labscope::api::LiveMonitorSocket>(
            telemetryRuntime->dbClient, sessions);
        app().registerController(liveMonitor);
    }
    else
    {
        labscope::api::MetricsController::registerRoutes({}, nullptr, nullptr, nullptr);
    }

    app().registerHandler(
        "/healthz",
        [](const HttpRequestPtr &,
           std::function<void(const HttpResponsePtr &)> &&callback) {
            auto response = HttpResponse::newHttpResponse();
            response->setBody("ok");
            callback(response);
        },
        {Get});

    if (!telemetryRuntime)
    app().registerHandler(
        "/api/v1/stats",
        [](const HttpRequestPtr &,
           std::function<void(const HttpResponsePtr &)> &&callback) {
            callback(HttpResponse::newHttpJsonResponse(telemetryHub().stats()));
        },
        {Get});

    if (!telemetryRuntime)
    app().registerHandler(
        "/api/v1/devices/{1}/latest",
        [](const HttpRequestPtr &,
           std::function<void(const HttpResponsePtr &)> &&callback,
           const std::string &deviceId) {
            auto value = telemetryHub().latest(deviceId);
            if (value)
                callback(HttpResponse::newHttpJsonResponse(*value));
            else
            {
                Json::Value error;
                error["error"] = "device not found";
                auto response = HttpResponse::newHttpJsonResponse(error);
                response->setStatusCode(k404NotFound);
                callback(response);
            }
        },
        {Get});

    if (!telemetryRuntime)
    app().registerHandler(
        "/api/v1/devices/{1}/history",
        [](const HttpRequestPtr &request,
           std::function<void(const HttpResponsePtr &)> &&callback,
           const std::string &deviceId) {
            const auto &params = request->getParameters();
            const auto limitIt = params.find("limit");
            const auto cursorIt = params.find("after_seq");
            const auto limit = limitIt == params.end()
                                   ? std::optional<int64_t>(50)
                                   : parseBoundedInt(limitIt->second, 1, 100);
            const auto cursor = cursorIt == params.end()
                                    ? std::optional<int64_t>(-1)
                                    : parseBoundedInt(cursorIt->second, -1, INT64_MAX);
            if (!limit || !cursor)
            {
                Json::Value error;
                error["error"] = "limit must be 1-100 and after_seq must be >= -1";
                auto response = HttpResponse::newHttpJsonResponse(error);
                response->setStatusCode(k400BadRequest);
                callback(response);
                return;
            }
            auto value = telemetryHub().history(deviceId, *cursor,
                                                 static_cast<size_t>(*limit));
            if (value)
                callback(HttpResponse::newHttpJsonResponse(*value));
            else
            {
                Json::Value error;
                error["error"] = "device not found";
                auto response = HttpResponse::newHttpJsonResponse(error);
                response->setStatusCode(k404NotFound);
                callback(response);
            }
        },
        {Get});

    app().setDocumentRoot("./web")
        .setHomePage("index.html")
        .setClientMaxWebSocketMessageSize(16 * 1024)
        .addListener(bindAddress, port);
    app().run();
    if (telemetryRuntime)
        telemetryRuntime->stop();
}
