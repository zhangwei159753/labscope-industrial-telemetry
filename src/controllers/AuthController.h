#pragma once

#include <drogon/orm/DbClient.h>

#include <memory>

namespace labscope::auth { class SessionStore; }

namespace labscope::api
{

class AuthController final
{
  public:
    static void registerRoutes(drogon::orm::DbClientPtr dbClient,
                               std::shared_ptr<labscope::auth::SessionStore> sessions);
};

}  // namespace labscope::api
