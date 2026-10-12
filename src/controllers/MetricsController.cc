#include "MetricsController.h"

#include "../ingest/IngestionService.h"
#include "../ingest/MqttConsumer.h"
#include "../alarms/AlarmProcessor.h"
#include "../metrics/TelemetryMetrics.h"
#include "../telemetry_hub.h"
#include "../auth/SessionFilter.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>

namespace labscope::api
{
namespace
{
using namespace drogon;

ingest::IngestionQueueStats queueSnapshot(ingest::IngestionService *service)
{
    return service ? service->stats() : ingest::IngestionQueueStats{};
}

uint64_t activeMonitors()
{
    const auto stats = telemetryHub().stats();
    return stats.get("monitors", Json::Value(0)).asUInt64();
}

std::string prometheusText(const metrics::MetricsSnapshot &snapshot,
                           const ingest::IngestionQueueStats &queue,
                           bool mqttConnected,
                           uint64_t monitors,
                           uint64_t sequenceGaps,
                           uint64_t activeAlarms,
                           bool databaseConnected)
{
    std::ostringstream out;
    out << "# HELP labscope_telemetry_received_total Valid telemetry messages received by the ingestion callback.\n"
        << "# TYPE labscope_telemetry_received_total counter\n"
        << "labscope_telemetry_received_total " << snapshot.received << '\n'
        << "# HELP labscope_telemetry_persisted_total Telemetry records committed to PostgreSQL.\n"
        << "# TYPE labscope_telemetry_persisted_total counter\n"
        << "labscope_telemetry_persisted_total " << snapshot.persisted << '\n'
        << "# HELP labscope_telemetry_duplicate_total Duplicate sequence records acknowledged after idempotent persistence.\n"
        << "# TYPE labscope_telemetry_duplicate_total counter\n"
        << "labscope_telemetry_duplicate_total " << snapshot.duplicates << '\n'
        << "# HELP labscope_telemetry_rejected_total Invalid or permanently rejected telemetry records.\n"
        << "# TYPE labscope_telemetry_rejected_total counter\n"
        << "labscope_telemetry_rejected_total " << snapshot.rejected << '\n'
        << "# HELP labscope_ingestion_queue_full_total Messages refused because the bounded ingestion queue was full.\n"
        << "# TYPE labscope_ingestion_queue_full_total counter\n"
        << "labscope_ingestion_queue_full_total " << snapshot.queueFull << '\n'
        << "# HELP labscope_ingestion_retry_total Persistence failures retained for device retry.\n"
        << "# TYPE labscope_ingestion_retry_total counter\n"
        << "labscope_ingestion_retry_total " << snapshot.retries << '\n'
        << "# HELP labscope_queue_to_commit_seconds Receive to database completion latency.\n"
        << "# TYPE labscope_queue_to_commit_seconds summary\n"
        << "labscope_queue_to_commit_seconds_count " << snapshot.commitLatencyCount << '\n'
        << "labscope_queue_to_commit_seconds_sum "
        << static_cast<double>(snapshot.commitLatencySumMs) / 1000.0 << '\n'
        << "# HELP labscope_queue_to_commit_seconds_max Maximum observed receive-to-commit latency.\n"
        << "# TYPE labscope_queue_to_commit_seconds_max gauge\n"
        << "labscope_queue_to_commit_seconds_max "
        << static_cast<double>(snapshot.commitLatencyMaxMs) / 1000.0 << '\n'
        << "# HELP labscope_ingestion_queue_depth Number of queued or in-flight messages.\n"
        << "# TYPE labscope_ingestion_queue_depth gauge\n"
        << "labscope_ingestion_queue_depth " << queue.depth << '\n'
        << "# HELP labscope_ingestion_queue_capacity Configured bounded ingestion capacity.\n"
        << "# TYPE labscope_ingestion_queue_capacity gauge\n"
        << "labscope_ingestion_queue_capacity " << queue.capacity << '\n'
        << "# HELP labscope_mqtt_connected Whether the MQTT broker connection is active.\n"
        << "# TYPE labscope_mqtt_connected gauge\n"
        << "labscope_mqtt_connected " << (mqttConnected ? 1 : 0) << '\n'
        << "# HELP labscope_mqtt_reconnect_total MQTT reconnect transitions after the first connection.\n"
        << "# TYPE labscope_mqtt_reconnect_total counter\n"
        << "labscope_mqtt_reconnect_total " << snapshot.brokerReconnects << '\n'
        << "# HELP labscope_websocket_clients Current authenticated monitor subscriptions.\n"
        << "# TYPE labscope_websocket_clients gauge\n"
        << "labscope_websocket_clients " << monitors << '\n'
        << "# HELP labscope_device_sequence_gaps_total Cumulative device sequence gaps recorded in PostgreSQL.\n"
        << "# TYPE labscope_device_sequence_gaps_total counter\n"
        << "labscope_device_sequence_gaps_total " << sequenceGaps << '\n'
        << "# HELP labscope_alarm_events_active Current active alarm events.\n"
        << "# TYPE labscope_alarm_events_active gauge\n"
        << "labscope_alarm_events_active " << activeAlarms << '\n'
        << "# HELP labscope_database_connected Whether the metrics query can reach PostgreSQL.\n"
        << "# TYPE labscope_database_connected gauge\n"
        << "labscope_database_connected " << (databaseConnected ? 1 : 0) << '\n';
    return out.str();
}
}  // namespace

void MetricsController::registerRoutes(orm::DbClientPtr dbClient,
                                       ingest::IngestionService *ingestion,
                                       ingest::MqttConsumer *mqtt,
                                       alarms::AlarmProcessor *alarms)
{
    app().registerHandler(
        "/metrics",
        [dbClient, ingestion, mqtt](const HttpRequestPtr &,
                                    std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            if (!dbClient)
            {
                auto response = HttpResponse::newHttpResponse();
                response->setContentTypeCode(CT_TEXT_PLAIN);
                response->setBody(prometheusText(metrics::telemetryMetrics().snapshot(),
                    queueSnapshot(ingestion), mqtt && mqtt->connected(), activeMonitors(), 0, 0, false));
                (*responseCallback)(response);
                return;
            }
            dbClient->execSqlAsync(
                "SELECT COALESCE((SELECT SUM(sequence_gap_count) FROM devices), 0)::bigint AS sequence_gaps, "
                "(SELECT count(*) FROM alarm_events WHERE state = 'active')::bigint AS active_alarms",
                [responseCallback, ingestion, mqtt](const orm::Result &rows) {
                    const auto gaps = rows.empty() ? 0 : rows[0]["sequence_gaps"].as<uint64_t>();
                    const auto active = rows.empty() ? 0 : rows[0]["active_alarms"].as<uint64_t>();
                    auto response = HttpResponse::newHttpResponse();
                    response->setContentTypeCode(CT_TEXT_PLAIN);
                    response->setBody(prometheusText(metrics::telemetryMetrics().snapshot(),
                        queueSnapshot(ingestion), mqtt && mqtt->connected(), activeMonitors(),
                        gaps, active, true));
                    (*responseCallback)(response);
                },
                [responseCallback, ingestion, mqtt](const orm::DrogonDbException &) {
                    auto response = HttpResponse::newHttpResponse();
                    response->setContentTypeCode(CT_TEXT_PLAIN);
                    response->setBody(prometheusText(metrics::telemetryMetrics().snapshot(),
                        queueSnapshot(ingestion), mqtt && mqtt->connected(), activeMonitors(),
                        0, 0, false));
                    (*responseCallback)(response);
                });
        },
        {Get});

    if (!dbClient) return;
    app().registerHandler(
        "/api/v1/system/health",
        [dbClient, ingestion, mqtt, alarms](const HttpRequestPtr &,
                                    std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            dbClient->execSqlAsync(
                "SELECT 1",
                [responseCallback, ingestion, mqtt, alarms](const orm::Result &) {
                    Json::Value body(Json::objectValue);
                    const bool alarmWorkerReady = alarms && alarms->isRunning();
                    body["status"] = mqtt && mqtt->connected() && alarmWorkerReady
                                          ? "healthy" : "degraded";
                    body["database"] = "healthy";
                    body["mqtt"] = mqtt && mqtt->connected() ? "connected" : "disconnected";
                    body["alarm_processor"] = alarmWorkerReady ? "running" : "stopped";
                    body["ingestion_queue_depth"] = Json::UInt64(queueSnapshot(ingestion).depth);
                    body["ingestion_queue_capacity"] = Json::UInt64(queueSnapshot(ingestion).capacity);
                    body["websocket_clients"] = Json::UInt64(activeMonitors());
                    body["retention_maintenance"] = "scheduled_hourly";
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                },
                [responseCallback, ingestion, mqtt, alarms](const orm::DrogonDbException &) {
                    Json::Value body(Json::objectValue);
                    body["status"] = "unhealthy";
                    body["database"] = "unavailable";
                    body["mqtt"] = mqtt && mqtt->connected() ? "connected" : "disconnected";
                    body["alarm_processor"] = alarms && alarms->isRunning() ? "running" : "stopped";
                    body["ingestion_queue_depth"] = Json::UInt64(queueSnapshot(ingestion).depth);
                    body["ingestion_queue_capacity"] = Json::UInt64(queueSnapshot(ingestion).capacity);
                    body["websocket_clients"] = Json::UInt64(activeMonitors());
                    body["retention_maintenance"] = "scheduled_hourly";
                    auto response = HttpResponse::newHttpJsonResponse(body);
                    response->setStatusCode(k503ServiceUnavailable);
                    (*responseCallback)(response);
                });
        },
        {Get, auth::SessionFilter::classTypeName()});
}

}  // namespace labscope::api
