#include "TelemetryController.h"
#include "../auth/SessionFilter.h"
#include "../auth/SiteAccessFilter.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace labscope::api
{
namespace
{
using namespace drogon;
constexpr int64_t kRawWindowMs = 24LL * 60 * 60 * 1000;
constexpr int64_t kAggregateWindowMs = 30LL * 24 * 60 * 60 * 1000;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool parseInteger(const std::string &text, int64_t minimum, int64_t maximum,
                  int64_t &value)
{
    if (text.empty())
        return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           value >= minimum && value <= maximum;
}

void sendError(const std::function<void(const HttpResponsePtr &)> &callback,
               HttpStatusCode status,
               const char *message)
{
    Json::Value body(Json::objectValue);
    body["error"] = message;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    callback(response);
}

Json::Value parseJsonObject(const std::string &text)
{
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string error;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &error) ||
        !value.isObject())
        return Json::Value(Json::objectValue);
    return value;
}

using ResponseCallback = std::function<void(const HttpResponsePtr &)>;
using SiteCallback = std::function<void(const std::string &)>;

void resolveSite(const drogon::orm::DbClientPtr &dbClient,
                 const std::string &deviceId,
                 const std::string &requestedSite,
                 const std::shared_ptr<ResponseCallback> &responseCallback,
                 SiteCallback resolved)
{
    dbClient->execSqlAsync(
        "SELECT site_id FROM devices WHERE device_id = $1 AND ($2 = '' OR site_id = $3) "
        "ORDER BY site_id LIMIT 2",
        [responseCallback, resolved = std::move(resolved)](
            const drogon::orm::Result &rows) mutable {
            if (rows.empty())
            {
                sendError(*responseCallback, k404NotFound, "device not found");
                return;
            }
            if (rows.size() > 1)
            {
                sendError(*responseCallback, k409Conflict,
                          "device_id exists in multiple sites; specify site_id");
                return;
            }
            resolved(rows[0]["site_id"].as<std::string>());
        },
        [responseCallback](const drogon::orm::DrogonDbException &) {
            sendError(*responseCallback, k503ServiceUnavailable,
                      "telemetry store is temporarily unavailable");
        },
        deviceId,
        requestedSite,
        requestedSite);
}

Json::Value rawSample(const drogon::orm::Row &row)
{
    Json::Value value(Json::objectValue);
    value["site_id"] = row["site_id"].as<std::string>();
    value["device_id"] = row["device_id"].as<std::string>();
    value["seq"] = Json::Int64(row["seq"].as<int64_t>());
    value["device_time_ms"] = Json::Int64(row["device_time_ms"].as<int64_t>());
    value["received_time_ms"] = Json::Int64(row["received_time_ms"].as<int64_t>());
    value["metrics"] = parseJsonObject(row["metrics"].as<std::string>());
    return value;
}

}  // namespace

void TelemetryController::registerRoutes(drogon::orm::DbClientPtr dbClient)
{
    app().registerHandler(
        "/api/v1/devices/{1}/latest",
        [dbClient](const HttpRequestPtr &request,
                   ResponseCallback &&callback,
                   const std::string &deviceId) {
            auto responseCallback = std::make_shared<ResponseCallback>(std::move(callback));
            const auto requestedSite = request->getParameter("site_id");
            resolveSite(dbClient, deviceId, requestedSite, responseCallback,
                        [dbClient, deviceId, responseCallback](const std::string &siteId) {
                dbClient->execSqlAsync(
                    "SELECT last_telemetry FROM devices "
                    "WHERE site_id = $1 AND device_id = $2",
                    [siteId, deviceId, responseCallback](const drogon::orm::Result &rows) {
                        if (rows.empty())
                        {
                            sendError(*responseCallback, k404NotFound, "device not found");
                            return;
                        }
                        Json::Value body(Json::objectValue);
                        body["site_id"] = siteId;
                        body["device_id"] = deviceId;
                        if (rows[0]["last_telemetry"].isNull())
                            body["latest"] = Json::nullValue;
                        else
                            body["latest"] =
                                parseJsonObject(rows[0]["last_telemetry"].as<std::string>());
                        (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                    },
                    [responseCallback](const drogon::orm::DrogonDbException &) {
                        sendError(*responseCallback, k503ServiceUnavailable,
                                  "latest telemetry is temporarily unavailable");
                    },
                    siteId,
                    deviceId);
            });
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/devices/{1}/history",
        [dbClient](const HttpRequestPtr &request,
                   ResponseCallback &&callback,
                   const std::string &deviceId) {
            auto responseCallback = std::make_shared<ResponseCallback>(std::move(callback));
            const auto &params = request->getParameters();
            auto parameter = [&params](const char *name) {
                auto found = params.find(name);
                return found == params.end() ? std::string{} : found->second;
            };
            const auto endNow = nowMs();
            int64_t fromMs = endNow - kRawWindowMs;
            int64_t toMs = endNow;
            int64_t limit = 100;
            int64_t cursor = -1;
            const auto fromText = parameter("from_ms");
            const auto toText = parameter("to_ms");
            const auto limitText = parameter("limit");
            const auto cursorText = parameter("cursor").empty()
                                        ? parameter("after_seq")
                                        : parameter("cursor");
            if ((!fromText.empty() && !parseInteger(fromText, 0, INT64_MAX, fromMs)) ||
                (!toText.empty() && !parseInteger(toText, 0, INT64_MAX, toMs)) ||
                (!limitText.empty() && !parseInteger(limitText, 1, 4096, limit)) ||
                (!cursorText.empty() && !parseInteger(cursorText, -1, INT64_MAX, cursor)) ||
                fromMs > toMs || toMs > endNow + 5 * 60 * 1000 ||
                toMs - fromMs > kAggregateWindowMs)
            {
                sendError(*responseCallback, k400BadRequest,
                          "invalid history range, cursor, or limit");
                return;
            }
            const bool aggregated = toMs - fromMs >= kRawWindowMs;
            if (!aggregated && limit > 1000)
            {
                sendError(*responseCallback, k400BadRequest,
                          "raw history limit must be between 1 and 1000");
                return;
            }
            if (!aggregated && cursor < -1)
            {
                sendError(*responseCallback, k400BadRequest, "cursor must be >= -1");
                return;
            }
            const auto requestedSite = parameter("site_id");
            resolveSite(dbClient, deviceId, requestedSite, responseCallback,
                        [dbClient, deviceId, responseCallback, fromMs, toMs, limit, cursor,
                         aggregated](const std::string &siteId) {
                if (!aggregated)
                {
                    dbClient->execSqlAsync(
                        "SELECT site_id, device_id, seq, "
                        "(extract(epoch FROM device_time) * 1000)::bigint AS device_time_ms, "
                        "(extract(epoch FROM received_at) * 1000)::bigint AS received_time_ms, "
                        "metrics::text AS metrics FROM telemetry_samples "
                        "WHERE site_id = $1 AND device_id = $2 "
                        "AND received_at >= to_timestamp($3::double precision / 1000.0) "
                        "AND received_at <= to_timestamp($4::double precision / 1000.0) "
                        "AND seq > $5 ORDER BY received_at, seq LIMIT $6",
                        [siteId, deviceId, responseCallback, limit, cursor](
                            const drogon::orm::Result &rows) {
                            Json::Value body(Json::objectValue);
                            body["site_id"] = siteId;
                            body["device_id"] = deviceId;
                            body["aggregated"] = false;
                            body["samples"] = Json::arrayValue;
                            const bool hasMore = rows.size() > static_cast<std::size_t>(limit);
                            const auto count = (std::min)(rows.size(), static_cast<std::size_t>(limit));
                            int64_t nextCursor = cursor;
                            for (std::size_t index = 0; index < count; ++index)
                            {
                                body["samples"].append(rawSample(rows[index]));
                                nextCursor = rows[index]["seq"].as<int64_t>();
                            }
                            body["next_cursor"] = Json::Int64(nextCursor);
                            body["next_after_seq"] = Json::Int64(nextCursor);
                            body["has_more"] = hasMore;
                            (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                        },
                        [responseCallback](const drogon::orm::DrogonDbException &) {
                            sendError(*responseCallback, k503ServiceUnavailable,
                                      "telemetry history is temporarily unavailable");
                        },
                        siteId,
                        deviceId,
                        static_cast<double>(fromMs),
                        static_cast<double>(toMs),
                        cursor,
                        limit + 1);
                    return;
                }

                dbClient->execSqlAsync(
                    "WITH buckets AS ("
                    " SELECT bucket_start FROM telemetry_1m WHERE site_id = $1 AND device_id = $2"
                    " AND bucket_start >= to_timestamp($3::double precision / 1000.0)"
                    " AND bucket_start <= to_timestamp($4::double precision / 1000.0)"
                    " AND ($5 < 0 OR (extract(epoch FROM bucket_start) * 1000)::bigint > $6)"
                    " GROUP BY bucket_start ORDER BY bucket_start LIMIT $7)"
                    " SELECT a.bucket_start,"
                    " (extract(epoch FROM a.bucket_start) * 1000)::bigint AS bucket_ms,"
                    " a.metric_name, a.min_value, a.max_value, a.sum_value, a.sample_count"
                    " FROM telemetry_1m a JOIN buckets b USING (bucket_start)"
                    " WHERE a.site_id = $8 AND a.device_id = $9"
                    " ORDER BY a.bucket_start, a.metric_name",
                    [siteId, deviceId, responseCallback, limit, cursor](
                        const drogon::orm::Result &rows) {
                        Json::Value body(Json::objectValue);
                        body["site_id"] = siteId;
                        body["device_id"] = deviceId;
                        body["aggregated"] = true;
                        body["samples"] = Json::arrayValue;
                        int64_t currentBucket = -1;
                        Json::Value sample(Json::objectValue);
                        bool hasMore = false;
                        for (const auto &row : rows)
                        {
                            const auto bucket = row["bucket_ms"].as<int64_t>();
                            if (bucket != currentBucket)
                            {
                                if (currentBucket >= 0)
                                    body["samples"].append(sample);
                                currentBucket = bucket;
                                sample = Json::Value(Json::objectValue);
                                sample["bucket_start_ms"] = Json::Int64(bucket);
                                sample["metrics"] = Json::objectValue;
                            }
                            const auto name = row["metric_name"].as<std::string>();
                            auto &metric = sample["metrics"][name];
                            metric["min"] = row["min_value"].as<double>();
                            metric["max"] = row["max_value"].as<double>();
                            metric["avg"] = row["sum_value"].as<double>() /
                                             static_cast<double>(row["sample_count"].as<int64_t>());
                            metric["count"] = Json::Int64(row["sample_count"].as<int64_t>());
                            if (body["samples"].size() >= static_cast<Json::ArrayIndex>(limit))
                                hasMore = true;
                        }
                        if (currentBucket >= 0)
                            body["samples"].append(sample);
                        if (body["samples"].size() > static_cast<Json::ArrayIndex>(limit))
                        {
                            body["samples"].removeIndex(body["samples"].size() - 1, nullptr);
                            hasMore = true;
                        }
                        const auto count = body["samples"].size();
                        const int64_t nextCursor = count == 0
                            ? cursor
                            : body["samples"][count - 1]["bucket_start_ms"].asInt64();
                        body["next_cursor"] = Json::Int64(nextCursor);
                        body["has_more"] = hasMore;
                        (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                    },
                    [responseCallback](const drogon::orm::DrogonDbException &) {
                        sendError(*responseCallback, k503ServiceUnavailable,
                                  "telemetry aggregates are temporarily unavailable");
                    },
                    siteId,
                    deviceId,
                    static_cast<double>(fromMs),
                    static_cast<double>(toMs),
                    cursor,
                    cursor,
                    limit + 1,
                    siteId,
                    deviceId);
            });
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});

    auto statsHandler = [dbClient](const HttpRequestPtr &request,
                   ResponseCallback &&callback) {
            auto responseCallback = std::make_shared<ResponseCallback>(std::move(callback));
            const auto siteId = request->getParameter("site_id");
            dbClient->execSqlAsync(
                "SELECT count(*) AS devices,"
                " count(*) FILTER (WHERE enabled) AS enabled_devices,"
                " count(*) FILTER (WHERE enabled AND last_seen_at >= clock_timestamp() - interval '60 seconds') AS online_devices,"
                " count(*) FILTER (WHERE enabled AND last_seen_at IS NOT NULL AND last_seen_at < clock_timestamp() - interval '60 seconds') AS stale_devices,"
                " (SELECT count(*) FROM telemetry_samples WHERE received_at >= clock_timestamp() - interval '60 seconds' AND ($1::text = '' OR site_id = $1)) AS samples_last_minute"
                " FROM devices WHERE ($1::text = '' OR site_id = $1)",
                [responseCallback](const drogon::orm::Result &rows) {
                    Json::Value body(Json::objectValue);
                    if (!rows.empty())
                    {
                        for (const char *field : {"devices", "enabled_devices", "online_devices",
                                                  "stale_devices", "samples_last_minute"})
                            body[field] = Json::Int64(rows[0][field].as<int64_t>());
                    }
                    body["ingest_messages_per_second"] =
                        body["samples_last_minute"].asInt64() / 60.0;
                    body["alarms_available"] = false;
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                },
                [responseCallback](const drogon::orm::DrogonDbException &) {
                    sendError(*responseCallback, k503ServiceUnavailable,
                              "telemetry statistics are temporarily unavailable");
                },
                siteId);
        };
    auto overviewHandler = statsHandler;
    app().registerHandler(
        "/api/v1/stats", std::move(statsHandler),
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});
    app().registerHandler(
        "/api/v1/overview", std::move(overviewHandler),
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});
}

}  // namespace labscope::api
