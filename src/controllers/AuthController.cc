#include "AuthController.h"

#include "../auth/Authorization.h"
#include "../auth/PasswordHasher.h"
#include "../auth/SessionFilter.h"
#include "../auth/SessionStore.h"
#include "../auth/SiteAccessFilter.h"

#include <drogon/drogon.h>
#include <sodium.h>

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace labscope::api
{
namespace
{
using namespace drogon;

using ResponseCallback = std::function<void(const HttpResponsePtr &)>;

void sendError(const ResponseCallback &callback, HttpStatusCode status, const char *message)
{
    Json::Value body(Json::objectValue);
    body["error"] = message;
    auto response = HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    callback(response);
}

bool validUsername(const std::string &username)
{
    if (username.size() < 3 || username.size() > 64 ||
        !std::isalnum(static_cast<unsigned char>(username.front())))
        return false;
    for (unsigned char ch : username)
        if (!std::isalnum(ch) && ch != '_' && ch != '.' && ch != '@' && ch != '-')
            return false;
    return true;
}

bool cookieSecure()
{
    if (const char *setting = std::getenv("LABSCOPE_COOKIE_SECURE"))
        return std::string(setting) == "1" || std::string(setting) == "true";
    const char *environment = std::getenv("LABSCOPE_ENV");
    return environment && std::string(environment) != "development";
}

Cookie sessionCookie(const std::string &token)
{
    Cookie cookie(labscope::auth::kSessionCookie, token);
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(cookieSecure());
    cookie.setSameSite(Cookie::SameSite::kLax);
    cookie.setMaxAge(8 * 60 * 60);
    return cookie;
}

Json::Value userJson(int64_t userId,
                     const std::string &username,
                     labscope::auth::Role role)
{
    Json::Value user(Json::objectValue);
    user["id"] = Json::Int64(userId);
    user["username"] = username;
    user["role"] = labscope::auth::roleName(role);
    return user;
}

Json::Value sessionJson(const labscope::auth::AuthContext &context)
{
    Json::Value body(Json::objectValue);
    body["user"] = userJson(context.userId, context.username, context.role);
    body["csrf_token"] = context.csrfToken;
    return body;
}
}  // namespace

void AuthController::registerRoutes(
    drogon::orm::DbClientPtr dbClient,
    std::shared_ptr<labscope::auth::SessionStore> sessions)
{
    app().registerFilter(std::make_shared<labscope::auth::SessionFilter>(sessions));
    app().registerFilter(std::make_shared<labscope::auth::SiteAccessFilter>(dbClient));
    const auto sessionFilterName = labscope::auth::SessionFilter::classTypeName();

    app().registerHandler(
        "/api/v1/auth/login",
        [dbClient, sessions](const HttpRequestPtr &request, ResponseCallback &&callback) {
            auto callbackPtr = std::make_shared<ResponseCallback>(std::move(callback));
            const auto body = request->getJsonObject();
            if (!body || !(*body)["username"].isString() ||
                !(*body)["password"].isString())
            {
                sendError(*callbackPtr, k400BadRequest, "username and password are required");
                return;
            }
            const auto username = (*body)["username"].asString();
            auto password = (*body)["password"].asString();
            (*body)["password"] = Json::Value("");
            const char *environment = std::getenv("LABSCOPE_ENV");
            const std::string_view environmentName = environment ? environment : "";
            const auto minimumPasswordLength =
                labscope::auth::PasswordHasher::minimumPasswordLengthForEnvironment(
                    environmentName);
            if (!validUsername(username) || password.size() < minimumPasswordLength ||
                password.size() > 1024)
            {
                sodium_memzero(password.data(), password.size());
                sendError(*callbackPtr, k401Unauthorized, "invalid username or password");
                return;
            }

            dbClient->execSqlAsync(
                "SELECT user_id, username, password_hash, role FROM web_users "
                "WHERE username = $1 AND enabled AND "
                "(locked_until IS NULL OR locked_until <= clock_timestamp())",
                [dbClient, sessions, callbackPtr, username, password = std::move(password)](
                    const drogon::orm::Result &rows) mutable {
                    if (rows.empty())
                    {
                        sodium_memzero(password.data(), password.size());
                        sendError(*callbackPtr, k401Unauthorized,
                                  "invalid username or password");
                        return;
                    }
                    const auto userId = rows[0]["user_id"].as<int64_t>();
                    const auto hash = rows[0]["password_hash"].as<std::string>();
                    const auto username = rows[0]["username"].as<std::string>();
                    auto role = labscope::auth::parseRole(rows[0]["role"].as<std::string>());
                    const bool passwordMatches =
                        labscope::auth::PasswordHasher::verifyPassword(hash, password);
                    sodium_memzero(password.data(), password.size());
                    if (!passwordMatches || !role)
                    {
                        dbClient->execSqlAsync(
                            "UPDATE web_users SET failed_login_count = failed_login_count + 1, "
                            "locked_until = CASE WHEN failed_login_count + 1 >= 5 "
                            "THEN clock_timestamp() + interval '5 minutes' ELSE locked_until END, "
                            "updated_at = clock_timestamp() WHERE user_id = $1",
                            [](const drogon::orm::Result &) {},
                            [](const drogon::orm::DrogonDbException &) {}, userId);
                        sendError(*callbackPtr, k401Unauthorized,
                                  "invalid username or password");
                        return;
                    }

                    dbClient->execSqlAsync(
                        "UPDATE web_users SET failed_login_count = 0, locked_until = NULL, "
                        "updated_at = clock_timestamp() WHERE user_id = $1",
                        [](const drogon::orm::Result &) {},
                        [](const drogon::orm::DrogonDbException &) {}, userId);
                    sessions->create(userId,
                        [callbackPtr, userId, username, role = *role](
                            std::optional<labscope::auth::SessionCredentials> credentials,
                            std::string error) {
                            if (!credentials)
                            {
                                sendError(*callbackPtr, k503ServiceUnavailable,
                                          "could not create a session");
                                return;
                            }
                            Json::Value body(Json::objectValue);
                            body["user"] = userJson(userId, username, role);
                            body["csrf_token"] = credentials->csrfToken;
                            auto response = HttpResponse::newHttpJsonResponse(body);
                            response->addCookie(sessionCookie(credentials->sessionToken));
                            (*callbackPtr)(response);
                        });
                },
                [callbackPtr](const drogon::orm::DrogonDbException &) {
                    sendError(*callbackPtr, k503ServiceUnavailable,
                              "authentication service unavailable");
                },
                username);
        },
        {Post});

    app().registerHandler(
        "/api/v1/auth/me",
        [](const HttpRequestPtr &request, ResponseCallback &&callback) {
            const auto *context = labscope::auth::Authorization::context(request);
            if (!context)
            {
                sendError(callback, k401Unauthorized, "authentication required");
                return;
            }
            callback(HttpResponse::newHttpJsonResponse(sessionJson(*context)));
        },
        {Get, sessionFilterName});

    app().registerHandler(
        "/api/v1/auth/logout",
        [sessions](const HttpRequestPtr &request, ResponseCallback &&callback) {
            const auto token = request->getCookie(labscope::auth::kSessionCookie);
            sessions->revoke(token, [callback = std::move(callback)](
                                         bool, std::string error) mutable {
                if (!error.empty())
                {
                    sendError(callback, k503ServiceUnavailable,
                              "authentication service unavailable");
                    return;
                }
                auto response = HttpResponse::newHttpJsonResponse(
                    Json::Value(Json::objectValue));
                Cookie expired(labscope::auth::kSessionCookie, "");
                expired.setPath("/");
                expired.setHttpOnly(true);
                expired.setSecure(cookieSecure());
                expired.setSameSite(Cookie::SameSite::kLax);
                expired.setMaxAge(0);
                response->addCookie(std::move(expired));
                callback(response);
            });
        },
        {Post, sessionFilterName});
}

}  // namespace labscope::api
