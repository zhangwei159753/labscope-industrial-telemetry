#include "SiteController.h"

#include "../auth/Authorization.h"
#include "../auth/SessionFilter.h"

#include <drogon/drogon.h>

#include <memory>

namespace labscope::api
{

void SiteController::registerRoutes(drogon::orm::DbClientPtr dbClient)
{
    drogon::app().registerHandler(
        "/api/v1/sites",
        [dbClient](const drogon::HttpRequestPtr &request,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto responseCallback = std::make_shared<
                std::function<void(const drogon::HttpResponsePtr &)>>(std::move(callback));
            const auto *context = labscope::auth::Authorization::context(request);
            if (!context)
            {
                Json::Value body(Json::objectValue);
                body["error"] = "authentication required";
                auto response = drogon::HttpResponse::newHttpJsonResponse(body);
                response->setStatusCode(drogon::k401Unauthorized);
                (*responseCallback)(response);
                return;
            }

            const bool isAdmin = context->role == labscope::auth::Role::Admin;
            const char *sql = isAdmin
                ? "SELECT s.site_id, s.display_name, s.timezone, s.enabled "
                  "FROM sites s WHERE s.enabled ORDER BY s.site_id"
                : "SELECT s.site_id, s.display_name, s.timezone, s.enabled "
                  "FROM sites s JOIN web_user_sites us ON us.site_id = s.site_id "
                  "WHERE us.user_id = $1 AND s.enabled ORDER BY s.site_id";

            auto success = [responseCallback](const drogon::orm::Result &rows) {
                Json::Value body(Json::objectValue);
                body["sites"] = Json::arrayValue;
                for (const auto &row : rows)
                {
                    Json::Value site(Json::objectValue);
                    site["site_id"] = row["site_id"].as<std::string>();
                    site["display_name"] = row["display_name"].as<std::string>();
                    site["timezone"] = row["timezone"].as<std::string>();
                    site["enabled"] = row["enabled"].as<bool>();
                    body["sites"].append(std::move(site));
                }
                (*responseCallback)(drogon::HttpResponse::newHttpJsonResponse(body));
            };
            auto failure = [responseCallback](const drogon::orm::DrogonDbException &) {
                Json::Value body(Json::objectValue);
                body["error"] = "site registry is temporarily unavailable";
                auto response = drogon::HttpResponse::newHttpJsonResponse(body);
                response->setStatusCode(drogon::k503ServiceUnavailable);
                (*responseCallback)(response);
            };
            if (isAdmin)
                dbClient->execSqlAsync(sql, std::move(success), std::move(failure));
            else
                dbClient->execSqlAsync(sql, std::move(success), std::move(failure),
                                       context->userId);
        },
        {drogon::Get, labscope::auth::SessionFilter::classTypeName()});
}

}  // namespace labscope::api
