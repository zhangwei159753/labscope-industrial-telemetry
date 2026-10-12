#include "LiveMonitorSocket.h"

#include "../auth/Authorization.h"
#include "../telemetry_hub.h"

#include <drogon/drogon.h>

#include <stdexcept>
#include <utility>

namespace labscope::api
{
namespace
{
struct MonitorContext { uint64_t id; };

void reject(const drogon::WebSocketConnectionPtr &connection,
            drogon::CloseCode code,
            const char *message)
{
    connection->shutdown(code, message);
}

void subscribe(const std::string &siteId,
               const std::string &deviceId,
               const drogon::WebSocketConnectionPtr &connection)
{
    const auto id = telemetryHub().addMonitor(siteId + "/" + deviceId, connection);
    connection->setContext(std::make_shared<MonitorContext>(MonitorContext{id}));
    Json::Value reply(Json::objectValue);
    reply["type"] = "subscribed";
    reply["site_id"] = siteId;
    reply["device_id"] = deviceId;
    reply["push_interval_ms"] = 50;
    connection->sendJson(reply);
}
}  // namespace

LiveMonitorSocket::LiveMonitorSocket(
    drogon::orm::DbClientPtr dbClient,
    std::shared_ptr<labscope::auth::SessionStore> sessions)
    : dbClient_(std::move(dbClient)), sessions_(std::move(sessions))
{
    if (!dbClient_ || !sessions_)
        throw std::invalid_argument("live monitor requires database and session services");
}

void LiveMonitorSocket::handleNewConnection(
    const drogon::HttpRequestPtr &request,
    const drogon::WebSocketConnectionPtr &connection)
{
    const auto siteId = request->getParameter("site_id");
    const auto deviceId = request->getParameter("device_id");
    if (!TelemetryHub::validDeviceId(siteId) || !TelemetryHub::validDeviceId(deviceId))
    {
        reject(connection, drogon::CloseCode::kViolation, "valid site_id and device_id are required");
        return;
    }
    const auto token = request->getCookie(labscope::auth::kSessionCookie);
    if (token.empty())
    {
        reject(connection, drogon::CloseCode::kViolation, "authentication required");
        return;
    }

    sessions_->resolve(token,
        [this, siteId, deviceId, connection](std::optional<labscope::auth::AuthContext> context,
                                            std::string error) {
            if (!error.empty())
            {
                reject(connection, drogon::CloseCode::kUnexpectedCondition,
                       "authentication service unavailable");
                return;
            }
            if (!context)
            {
                reject(connection, drogon::CloseCode::kViolation,
                       "session expired or revoked");
                return;
            }
            if (context->role == labscope::auth::Role::Admin)
            {
                subscribe(siteId, deviceId, connection);
                return;
            }
            dbClient_->execSqlAsync(
                "SELECT EXISTS (SELECT 1 FROM web_user_sites "
                "WHERE user_id = $1 AND site_id = $2)",
                [siteId, deviceId, connection](const drogon::orm::Result &rows) {
                    if (rows.empty() || !rows[0][0].as<bool>())
                    {
                        reject(connection, drogon::CloseCode::kViolation,
                               "site access denied");
                        return;
                    }
                    subscribe(siteId, deviceId, connection);
                },
                [connection](const drogon::orm::DrogonDbException &) {
                    reject(connection, drogon::CloseCode::kUnexpectedCondition,
                           "site access service unavailable");
                },
                context->userId,
                siteId);
        });
}

void LiveMonitorSocket::handleNewMessage(
    const drogon::WebSocketConnectionPtr &connection,
    std::string &&,
    const drogon::WebSocketMessageType &type)
{
    if (type == drogon::WebSocketMessageType::Text)
    {
        Json::Value response(Json::objectValue);
        response["type"] = "error";
        response["reason"] = "monitor socket is read-only";
        connection->sendJson(response);
    }
}

void LiveMonitorSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr &connection)
{
    auto context = connection->getContext<MonitorContext>();
    if (context)
        telemetryHub().removeMonitor(context->id);
}

}  // namespace labscope::api
