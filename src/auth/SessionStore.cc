#include "SessionStore.h"

#include "PasswordHasher.h"

#include <drogon/orm/Exception.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace labscope::auth
{
using namespace drogon::orm;

SessionStore::SessionStore(DbClientPtr dbClient) : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
        throw std::invalid_argument("session store requires a database client");
}

void SessionStore::create(int64_t userId, CreateCallback callback) const
{
    auto credentials = std::make_shared<SessionCredentials>(
        SessionCredentials{PasswordHasher::randomToken(), PasswordHasher::randomToken(), {}});
    auto callbackPtr = std::make_shared<CreateCallback>(std::move(callback));
    const auto tokenHash = PasswordHasher::hashToken(credentials->sessionToken);
    dbClient_->execSqlAsync(
        "DELETE FROM web_sessions WHERE expires_at < clock_timestamp() - interval '7 days' "
        "OR revoked_at < clock_timestamp() - interval '7 days'",
        [](const Result &) {},
        [](const DrogonDbException &) {});
    dbClient_->execSqlAsync(
        "INSERT INTO web_sessions (session_hash, user_id, csrf_token, expires_at) "
        "VALUES ($1, $2, $3, clock_timestamp() + interval '8 hours') RETURNING expires_at",
        [callbackPtr, credentials](const Result &rows) mutable {
            if (rows.empty())
            {
                (*callbackPtr)(std::nullopt, "session insert returned no row");
                return;
            }
            credentials->expiresAt = rows[0]["expires_at"].as<std::string>();
            (*callbackPtr)(std::move(*credentials), {});
        },
        [callbackPtr](const DrogonDbException &) mutable {
            (*callbackPtr)(std::nullopt, "session database operation failed");
        },
        tokenHash,
        userId,
        credentials->csrfToken);
}

void SessionStore::resolve(const std::string &sessionToken,
                           ResolveCallback callback) const
{
    if (sessionToken.size() != 64)
    {
        callback(std::nullopt, {});
        return;
    }
    std::string tokenHash;
    try
    {
        tokenHash = PasswordHasher::hashToken(sessionToken);
    }
    catch (...)
    {
        callback(std::nullopt, "session token hashing failed");
        return;
    }
    auto callbackPtr = std::make_shared<ResolveCallback>(std::move(callback));
    dbClient_->execSqlAsync(
        "SELECT u.user_id, u.username, u.role, s.csrf_token "
        "FROM web_sessions s JOIN web_users u ON u.user_id = s.user_id "
        "WHERE s.session_hash = $1 AND s.revoked_at IS NULL "
        "AND s.expires_at > clock_timestamp() AND u.enabled",
        [callbackPtr](const Result &rows) mutable {
            if (rows.empty())
            {
                (*callbackPtr)(std::nullopt, {});
                return;
            }
            auto role = parseRole(rows[0]["role"].as<std::string>());
            if (!role)
            {
                (*callbackPtr)(std::nullopt, "user has an invalid role");
                return;
            }
            AuthContext context;
            context.userId = rows[0]["user_id"].as<int64_t>();
            context.username = rows[0]["username"].as<std::string>();
            context.role = *role;
            context.csrfToken = rows[0]["csrf_token"].as<std::string>();
            (*callbackPtr)(std::move(context), {});
        },
        [callbackPtr](const DrogonDbException &) mutable {
            (*callbackPtr)(std::nullopt, "session database operation failed");
        },
        tokenHash);
}

void SessionStore::revoke(const std::string &sessionToken,
                          RevokeCallback callback) const
{
    if (sessionToken.size() != 64)
    {
        callback(false, {});
        return;
    }
    const auto tokenHash = PasswordHasher::hashToken(sessionToken);
    auto callbackPtr = std::make_shared<RevokeCallback>(std::move(callback));
    dbClient_->execSqlAsync(
        "UPDATE web_sessions SET revoked_at = clock_timestamp() "
        "WHERE session_hash = $1 AND revoked_at IS NULL",
        [callbackPtr](const Result &rows) mutable {
            (*callbackPtr)(rows.affectedRows() > 0, {});
        },
        [callbackPtr](const DrogonDbException &) mutable {
            (*callbackPtr)(false, "session database operation failed");
        },
        tokenHash);
}

}  // namespace labscope::auth
