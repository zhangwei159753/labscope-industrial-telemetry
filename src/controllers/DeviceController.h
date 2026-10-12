#pragma once

#include <drogon/orm/DbClient.h>

namespace labscope::api
{

class DeviceController final
{
  public:
    static void registerRoutes(drogon::orm::DbClientPtr dbClient);
};

}  // namespace labscope::api
