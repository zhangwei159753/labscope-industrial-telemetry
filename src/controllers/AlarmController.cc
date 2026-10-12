#include "AlarmController.h"

#include "../alarms/AlarmRepository.h"
#include "../auth/Authorization.h"
#include "../auth/SessionFilter.h"
#include "../auth/SiteAccessFilter.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace labscope::api
{
namespace
{
using namespace drogon;

void sendError(const std::function<void(const HttpResponsePtr &)> &callback,
               HttpStatusCode status, const std::string &message)
{
    Json::Value body(Json::objectValue);
    body["error"] = message;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    callback(response);
}

bool parseInt64(const std::string &text, int64_t minimum, int64_t maximum,
                int64_t &value)
{
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           value >= minimum && value <= maximum;
}

Json::Value eventJson(const orm::Row &row)
{
    Json::Value event(Json::objectValue);
    event["id"] = std::to_string(row["event_id"].as<int64_t>());
    event["rule_id"] = std::to_string(row["rule_id"].as<int64_t>());
    event["site_id"] = row["site_id"].as<std::string>();
    event["device_id"] = row["device_id"].as<std::string>();
    event["metric"] = row["metric_name"].as<std::string>();
    event["severity"] = row["severity"].as<std::string>();
    event["state"] = row["state"].as<std::string>();
    event["seq"] = Json::Int64(row["trigger_seq"].as<int64_t>());
    event["value"] = row["trigger_value"].as<double>();
    event["triggered_at"] = row["triggered_at"].as<std::string>();
    if (row["acknowledged_by"].isNull())
        event["acknowledged_by"] = Json::nullValue;
    else
        event["acknowledged_by"] = row["acknowledged_username"].as<std::string>();
    if (row["acknowledged_at"].isNull())
        event["acknowledged_at"] = Json::nullValue;
    else
        event["acknowledged_at"] = row["acknowledged_at"].as<std::string>();
    event["acknowledgment_remark"] = row["acknowledgment_remark"].isNull()
                                          ? Json::Value(Json::nullValue)
                                          : Json::Value(row["acknowledgment_remark"].as<std::string>());
    event["recovered_at"] = row["recovered_at"].isNull()
                                 ? Json::Value(Json::nullValue)
                                 : Json::Value(row["recovered_at"].as<std::string>());
    return event;
}

Json::Value ruleJson(const orm::Row &row)
{
    Json::Value rule(Json::objectValue);
    rule["id"] = std::to_string(row["rule_id"].as<int64_t>());
    rule["site_id"] = row["site_id"].as<std::string>();
    rule["device_id"] = row["device_id"].as<std::string>();
    rule["metric"] = row["metric_name"].as<std::string>();
    rule["direction"] = row["direction"].as<std::string>();
    rule["threshold"] = row["threshold"].as<double>();
    rule["duration_ms"] = Json::Int64(row["duration_ms"].as<int64_t>());
    rule["hysteresis"] = row["hysteresis"].as<double>();
    rule["cooldown_ms"] = Json::Int64(row["cooldown_ms"].as<int64_t>());
    rule["severity"] = row["severity"].as<std::string>();
    rule["enabled"] = row["enabled"].as<bool>();
    return rule;
}
}  // namespace

void AlarmController::registerRoutes(drogon::orm::DbClientPtr dbClient)
{
    auto repository = std::make_shared<labscope::alarms::AlarmRepository>(dbClient);
    app().registerHandler(
        "/api/v1/alarms/rules",
        [dbClient](const HttpRequestPtr &request,
                   std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto &params = request->getParameters();
            auto parameter = [&params](const char *name) {
                const auto it = params.find(name);
                return it == params.end() ? std::string{} : it->second;
            };
            int64_t limit = 100;
            int64_t cursor = 0;
            const auto limitText = parameter("limit");
            const auto cursorText = parameter("cursor");
            if ((!limitText.empty() && !parseInt64(limitText, 1, 100, limit)) ||
                (!cursorText.empty() && !parseInt64(cursorText, 1,
                    (std::numeric_limits<int64_t>::max)(), cursor)))
            {
                sendError(*responseCallback, k400BadRequest, "invalid alarm rule cursor or limit");
                return;
            }
            const auto siteId = parameter("site_id");
            dbClient->execSqlAsync(
                "SELECT rule_id, site_id, device_id, metric_name, direction, threshold, "
                "duration_ms, hysteresis, cooldown_ms, severity, enabled "
                "FROM alarm_rules WHERE ($1 = '' OR site_id = $2) "
                "AND ($3::bigint = 0 OR rule_id > $4::bigint) "
                "ORDER BY rule_id LIMIT $5",
                [responseCallback, limit](const orm::Result &rows) {
                    Json::Value body(Json::objectValue);
                    body["rules"] = Json::arrayValue;
                    const bool hasMore = rows.size() > static_cast<std::size_t>(limit);
                    const auto count = (std::min)(rows.size(), static_cast<std::size_t>(limit));
                    for (std::size_t i = 0; i < count; ++i)
                        body["rules"].append(ruleJson(rows[i]));
                    body["next_cursor"] = Json::nullValue;
                    if (hasMore && count > 0)
                        body["next_cursor"] = std::to_string(
                            rows[count - 1]["rule_id"].as<int64_t>());
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                },
                [responseCallback](const orm::DrogonDbException &) {
                    sendError(*responseCallback, k503ServiceUnavailable,
                              "alarm rule store is temporarily unavailable");
                },
                siteId, siteId, cursor, cursor, limit + 1);
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/alarms/rules",
        [repository](const HttpRequestPtr &request,
                     std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto *context = labscope::auth::Authorization::context(request);
            if (!labscope::auth::Authorization::requireRole(
                    request, labscope::auth::Role::Admin))
            {
                sendError(*responseCallback, k403Forbidden,
                          context ? "administrator role required" : "authentication required");
                return;
            }
            const auto body = request->getJsonObject();
            if (!body || !body->isObject())
            {
                sendError(*responseCallback, k400BadRequest, "JSON object is required");
                return;
            }
            const auto siteId = (*body)["site_id"].asString();
            const auto deviceId = (*body)["device_id"].asString();
            const auto metric = (*body)["metric"].asString();
            const auto directionText = (*body)["direction"].asString();
            const auto severityText = (*body)["severity"].asString();
            const auto thresholdValue = (*body)["threshold"];
            const auto hysteresisValue = (*body)["hysteresis"];
            const Json::Value durationValue = body->get("duration_ms", Json::Value(0));
            const Json::Value cooldownValue = body->get("cooldown_ms", Json::Value(0));
            if (!thresholdValue.isNumeric() || !durationValue.isIntegral() ||
                !cooldownValue.isIntegral() ||
                (!hysteresisValue.isNull() && !hysteresisValue.isNumeric()))
            {
                sendError(*responseCallback, k400BadRequest,
                          "threshold, duration, cooldown, and hysteresis must be numeric");
                return;
            }
            const int64_t durationMs = durationValue.asInt64();
            const int64_t cooldownMs = cooldownValue.asInt64();
            const double threshold = thresholdValue.asDouble();
            const double hysteresis = hysteresisValue.isNumeric()
                                          ? hysteresisValue.asDouble() : 0.0;
            if (siteId.empty() || deviceId.empty() || metric.empty() || metric.size() > 128 ||
                (directionText != "above" && directionText != "below") ||
                !std::isfinite(threshold) ||
                (hysteresisValue.isNumeric() &&
                 (!std::isfinite(hysteresis) || hysteresis < 0)) ||
                !durationValue.isIntegral() || durationMs < 0 || durationMs > 86400000 ||
                !cooldownValue.isIntegral() || cooldownMs < 0 || cooldownMs > 604800000 ||
                (severityText != "info" && severityText != "warning" &&
                 severityText != "critical"))
            {
                sendError(*responseCallback, k400BadRequest,
                          "invalid alarm rule fields or bounds");
                return;
            }
            labscope::alarms::CreateRule rule;
            rule.siteId = siteId;
            rule.deviceId = deviceId;
            rule.metric = metric;
            rule.direction = directionText == "below"
                                 ? labscope::alarms::ThresholdDirection::Below
                                 : labscope::alarms::ThresholdDirection::Above;
            rule.threshold = threshold;
            rule.durationMs = durationMs;
            rule.hysteresis = hysteresis;
            rule.cooldownMs = cooldownMs;
            rule.severity = severityText == "critical"
                                ? labscope::alarms::AlarmSeverity::Critical
                            : severityText == "info"
                                ? labscope::alarms::AlarmSeverity::Info
                                : labscope::alarms::AlarmSeverity::Warning;
            repository->createRule(rule, context->userId,
                [responseCallback](labscope::alarms::CreateRuleResult result) {
                    if (!result.succeeded)
                    {
                        sendError(*responseCallback,
                                  result.error.find("foreign key") != std::string::npos
                                      ? k404NotFound : k503ServiceUnavailable,
                                  result.error.empty() ? "alarm rule creation failed" : result.error);
                        return;
                    }
                    Json::Value responseBody(Json::objectValue);
                    responseBody["rule_id"] = std::to_string(result.ruleId);
                    auto response = HttpResponse::newHttpJsonResponse(responseBody);
                    response->setStatusCode(k201Created);
                    (*responseCallback)(response);
                });
        },
        {Post, labscope::auth::SessionFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/alarms/rules/{1}",
        [dbClient](const HttpRequestPtr &request,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   const std::string &ruleIdText) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto *context = labscope::auth::Authorization::context(request);
            if (!labscope::auth::Authorization::requireRole(
                    request, labscope::auth::Role::Admin))
            {
                sendError(*responseCallback, k403Forbidden,
                          context ? "administrator role required" : "authentication required");
                return;
            }
            int64_t ruleId = 0;
            const auto body = request->getJsonObject();
            if (!parseInt64(ruleIdText, 1, (std::numeric_limits<int64_t>::max)(), ruleId) ||
                !body || !(*body)["enabled"].isBool())
            {
                sendError(*responseCallback, k400BadRequest,
                          "rule id and boolean enabled field are required");
                return;
            }
            auto transactionError = std::make_shared<std::string>();
            dbClient->newTransactionAsync(
                [responseCallback, transactionError, ruleId,
                 enabled = (*body)["enabled"].asBool(), userId = context->userId](
                    const std::shared_ptr<orm::Transaction> &transaction) {
                    if (!transaction)
                    {
                        sendError(*responseCallback, k503ServiceUnavailable,
                                  "database transaction unavailable");
                        return;
                    }
                    transaction->setCommitCallback(
                        [responseCallback, transactionError](bool committed) {
                            if (!committed || !transactionError->empty())
                            {
                                const auto missing = *transactionError == "alarm rule not found";
                                sendError(*responseCallback,
                                    missing ? k404NotFound : k503ServiceUnavailable,
                                    transactionError->empty() ? "alarm rule update did not commit" : *transactionError);
                                return;
                            }
                            Json::Value body(Json::objectValue);
                            body["ok"] = true;
                            (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                        });
                    transaction->execSqlAsync(
                        "UPDATE alarm_rules SET enabled = $1, updated_at = clock_timestamp() "
                        "WHERE rule_id = $2 RETURNING site_id, device_id",
                        [transaction, transactionError, ruleId, enabled, userId](
                            const orm::Result &rows) {
                            if (rows.empty())
                            {
                                *transactionError = "alarm rule not found";
                                transaction->rollback();
                                return;
                            }
                            const auto siteId = rows[0]["site_id"].as<std::string>();
                            const auto deviceId = rows[0]["device_id"].as<std::string>();
                            transaction->execSqlAsync(
                                "INSERT INTO audit_log (user_id, action, entity_type, entity_id, site_id, metadata) "
                                "VALUES ($1, 'alarm_rule.set_enabled', 'alarm_rule', $2, $3, "
                                "jsonb_build_object('device_id', $4::text, "
                                "'enabled', $5::boolean))",
                                [transaction](const orm::Result &) {},
                                [transaction, transactionError](const orm::DrogonDbException &error) {
                                    *transactionError = error.base().what();
                                    transaction->rollback();
                                },
                                userId, std::to_string(ruleId), siteId, deviceId, enabled);
                        },
                        [transaction, transactionError](const orm::DrogonDbException &error) {
                            *transactionError = error.base().what();
                            transaction->rollback();
                        },
                        enabled, ruleId);
                },
                orm::TransactionType::Deferred);
        },
        {Patch, labscope::auth::SessionFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/alarms/events",
        [dbClient](const HttpRequestPtr &request,
                   std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto &params = request->getParameters();
            auto parameter = [&params](const char *name) {
                const auto it = params.find(name);
                return it == params.end() ? std::string{} : it->second;
            };
            int64_t limit = 50;
            const auto limitText = parameter("limit");
            int64_t cursor = 0;
            const auto cursorText = parameter("cursor");
            const auto state = parameter("state");
            const auto severity = parameter("severity");
            if ((!limitText.empty() && !parseInt64(limitText, 1, 100, limit)) ||
                (!cursorText.empty() && !parseInt64(cursorText, 1,
                    (std::numeric_limits<int64_t>::max)(), cursor)) ||
                (!state.empty() && state != "active" && state != "acknowledged" &&
                 state != "recovered") ||
                (!severity.empty() && severity != "info" && severity != "warning" &&
                 severity != "critical"))
            {
                sendError(*responseCallback, k400BadRequest, "invalid alarm event filter");
                return;
            }
            const auto siteId = parameter("site_id");
            const auto deviceId = parameter("device_id");
            const auto sql = R"SQL(
                SELECT e.event_id, e.rule_id, e.site_id, e.device_id, e.metric_name,
                       e.severity, e.state, e.trigger_seq, e.trigger_value,
                       e.triggered_at::text AS triggered_at,
                       e.acknowledged_by, u.username AS acknowledged_username,
                       e.acknowledged_at::text AS acknowledged_at,
                       e.acknowledgment_remark, e.recovered_at::text AS recovered_at
                FROM alarm_events e
                LEFT JOIN web_users u ON u.user_id = e.acknowledged_by
                WHERE ($1 = '' OR e.site_id = $2)
                  AND ($3 = '' OR e.device_id = $4)
                  AND ($5 = '' OR e.state = $6)
                  AND ($7 = '' OR e.severity = $8)
                  AND ($9::bigint = 0 OR e.event_id < $10::bigint)
                ORDER BY e.event_id DESC LIMIT $11
            )SQL";
            dbClient->execSqlAsync(
                sql,
                [responseCallback, limit](const orm::Result &rows) {
                    Json::Value body(Json::objectValue);
                    body["events"] = Json::arrayValue;
                    const bool hasMore = rows.size() > static_cast<std::size_t>(limit);
                    const auto count = (std::min)(rows.size(), static_cast<std::size_t>(limit));
                    for (std::size_t i = 0; i < count; ++i)
                        body["events"].append(eventJson(rows[i]));
                    body["next_cursor"] = Json::nullValue;
                    if (hasMore && count > 0)
                    body["next_cursor"] = std::to_string(
                        rows[count - 1]["event_id"].as<int64_t>());
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                },
                [responseCallback](const orm::DrogonDbException &) {
                    sendError(*responseCallback, k503ServiceUnavailable,
                              "alarm event store is temporarily unavailable");
                },
                siteId, siteId, deviceId, deviceId, state, state,
                severity, severity, cursor, cursor, limit + 1);
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/alarms/events/{1}/ack",
        [repository](const HttpRequestPtr &request,
                     std::function<void(const HttpResponsePtr &)> &&callback,
                     const std::string &eventIdText) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto *context = labscope::auth::Authorization::context(request);
            if (!labscope::auth::Authorization::requireRole(
                    request, labscope::auth::Role::Operator))
            {
                sendError(*responseCallback, k403Forbidden,
                          context ? "operator role required" : "authentication required");
                return;
            }
            int64_t eventId = 0;
            if (!parseInt64(eventIdText, 1, (std::numeric_limits<int64_t>::max)(), eventId))
            {
                sendError(*responseCallback, k400BadRequest, "invalid event id");
                return;
            }
            const auto body = request->getJsonObject();
            const auto remark = body && (*body)["remark"].isString()
                                    ? (*body)["remark"].asString() : std::string{};
            if (remark.size() > 512)
            {
                sendError(*responseCallback, k400BadRequest, "remark must be at most 512 bytes");
                return;
            }
            const auto siteId = request->getParameter("site_id");
            repository->acknowledge(eventId, siteId, context->userId, remark,
                [responseCallback](labscope::alarms::AcknowledgeResult result) {
                    if (!result.succeeded)
                    {
                        sendError(*responseCallback,
                                  result.error.find("not found or is no longer active") != std::string::npos
                                      ? k409Conflict : k503ServiceUnavailable,
                                  result.error.empty() ? "alarm acknowledgment failed" : result.error);
                        return;
                    }
                    Json::Value body(Json::objectValue);
                    body["ok"] = true;
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                });
        },
        {Post, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});
}

}  // namespace labscope::api
