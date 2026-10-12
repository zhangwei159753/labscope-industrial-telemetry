#pragma once

#include "Authorization.h"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace labscope::auth
{

struct SessionCredentials
{
    std::string sessionToken;
    std::string csrfToken;
    std::string expiresAt;
};

class SessionStore final
{
  public:
    using CreateCallback = std::function<void(std::optional<SessionCredentials>, std::string)>;
    using ResolveCallback = std::function<void(std::optional<AuthContext>, std::string)>;
    using RevokeCallback = std::function<void(bool, std::string)>;

    explicit SessionStore(drogon::orm::DbClientPtr dbClient);
    void create(int64_t userId, CreateCallback callback) const;
    void resolve(const std::string &sessionToken, ResolveCallback callback) const;
    void revoke(const std::string &sessionToken, RevokeCallback callback) const;

  private:
    drogon::orm::DbClientPtr dbClient_;
};

}  // namespace labscope::auth
