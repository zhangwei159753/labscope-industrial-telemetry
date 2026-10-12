#include "DeviceController.h"
#include "../auth/SessionFilter.h"
#include "../auth/SiteAccessFilter.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace labscope::api
{
namespace
{
using namespace drogon;

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

bool parseLimit(const std::string &text, int64_t &value)
{
    if (text.empty())
        return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           value >= 1 && value <= 100;
}

std::optional<std::pair<std::string, std::string>> parseCursor(const std::string &cursor)
{
    if (cursor.empty())
        return std::pair<std::string, std::string>{};
    const auto slash = cursor.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= cursor.size() ||
        cursor.find('/', slash + 1) != std::string::npos)
        return std::nullopt;
    return std::pair<std::string, std::string>{cursor.substr(0, slash),
                                               cursor.substr(slash + 1)};
}

Json::Value deviceJson(const drogon::orm::Row &row)
{
    Json::Value value(Json::objectValue);
    value["site_id"] = row["site_id"].as<std::string>();
    value["device_id"] = row["device_id"].as<std::string>();
    value["display_name"] = row["display_name"].as<std::string>();
    value["model"] = row["model"].as<std::string>();
    value["enabled"] = row["enabled"].as<bool>();
    value["last_seq"] = Json::Int64(row["last_seq"].as<int64_t>());
    value["sequence_gap_count"] =
        Json::Int64(row["sequence_gap_count"].as<int64_t>());
    value["status"] = row["status"].as<std::string>();
    if (row["last_seen_at"].isNull())
        value["last_seen_at"] = Json::nullValue;
    else
        value["last_seen_at"] = row["last_seen_at"].as<std::string>();
    return value;
}

constexpr char kDeviceProjection[] = R"SQL(
    SELECT d.site_id, d.device_id, d.display_name, d.model, d.enabled,
           d.last_seq, d.sequence_gap_count, d.last_seen_at,
           CASE WHEN NOT d.enabled THEN 'disabled'
                WHEN d.last_seen_at IS NULL THEN 'never_seen'
                WHEN d.last_seen_at >= clock_timestamp() - interval '60 seconds'
                    THEN 'online'
                ELSE 'stale' END AS status
    FROM devices d
)SQL";

}  // namespace

void DeviceController::registerRoutes(drogon::orm::DbClientPtr dbClient)
{
    app().registerHandler(
        "/api/v1/devices",
        [dbClient](const HttpRequestPtr &request,
                   std::function<void(const HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto &params = request->getParameters();
            auto parameter = [&params](const char *key) {
                auto found = params.find(key);
                return found == params.end() ? std::string{} : found->second;
            };
            int64_t limit = 50;
            const auto limitText = parameter("limit");
            if (!limitText.empty() && !parseLimit(limitText, limit))
            {
                sendError(*responseCallback, k400BadRequest,
                          "limit must be between 1 and 100");
                return;
            }
            const auto siteId = parameter("site_id");
            const auto status = parameter("status");
            if (!status.empty() && status != "online" && status != "stale" &&
                status != "never_seen" && status != "disabled")
            {
                sendError(*responseCallback, k400BadRequest,
                          "unsupported device status filter");
                return;
            }
            const auto cursor = parseCursor(parameter("cursor"));
            if (!cursor)
            {
                sendError(*responseCallback, k400BadRequest,
                          "cursor must be site_id/device_id");
                return;
            }
            std::string sql = "WITH registry AS (";
            sql += kDeviceProjection;
            sql += R"SQL()
                SELECT * FROM registry
                WHERE ($1 = '' OR site_id = $2)
                  AND ($3 = '' OR (site_id, device_id) > ($4, $5))
                  AND ($6 = '' OR status = $7)
                ORDER BY site_id, device_id
                LIMIT $8
            )SQL";
            dbClient->execSqlAsync(
                sql,
                [responseCallback, limit](const drogon::orm::Result &rows) mutable {
                    Json::Value body(Json::objectValue);
                    body["devices"] = Json::arrayValue;
                    const bool hasMore = rows.size() > static_cast<std::size_t>(limit);
                    const auto count = (std::min)(rows.size(), static_cast<std::size_t>(limit));
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        body["devices"].append(deviceJson(rows[index]));
                    }
                    body["next_cursor"] = Json::nullValue;
                    if (hasMore && count > 0)
                    {
                        const auto &last = rows[count - 1];
                        body["next_cursor"] = last["site_id"].as<std::string>() + "/" +
                                               last["device_id"].as<std::string>();
                    }
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(body));
                },
                [responseCallback](const drogon::orm::DrogonDbException &) mutable {
                    sendError(*responseCallback, k503ServiceUnavailable,
                              "device registry is temporarily unavailable");
                },
                siteId,
                siteId,
                cursor->first,
                cursor->first,
                cursor->second,
                status,
                status,
                limit + 1);
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});

    app().registerHandler(
        "/api/v1/devices/{1}",
        [dbClient](const HttpRequestPtr &request,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   const std::string &deviceId) {
            auto responseCallback = std::make_shared<
                std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            const auto siteId = request->getParameter("site_id");
            std::string sql = "WITH registry AS (";
            sql += kDeviceProjection;
            sql += ") SELECT * FROM registry WHERE device_id = $1 AND ($2 = '' OR site_id = $3) "
                   "ORDER BY site_id LIMIT 2";
            dbClient->execSqlAsync(
                sql,
                [responseCallback](const drogon::orm::Result &rows) mutable {
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
                    (*responseCallback)(HttpResponse::newHttpJsonResponse(deviceJson(rows[0])));
                },
                [responseCallback](const drogon::orm::DrogonDbException &) mutable {
                    sendError(*responseCallback, k503ServiceUnavailable,
                              "device registry is temporarily unavailable");
                },
                deviceId,
                siteId,
                siteId);
        },
        {Get, labscope::auth::SessionFilter::classTypeName(),
         labscope::auth::SiteAccessFilter::classTypeName()});
}

}  // namespace labscope::api
