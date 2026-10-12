#pragma once

#include "../auth/SessionStore.h"
#include "../auth/SessionFilter.h"

#include <drogon/WebSocketController.h>
#include <drogon/orm/DbClient.h>

#include <memory>

namespace labscope::api
{

class LiveMonitorSocket final : public drogon::WebSocketController<LiveMonitorSocket, false>
{
  public:
    LiveMonitorSocket(drogon::orm::DbClientPtr dbClient,
                      std::shared_ptr<labscope::auth::SessionStore> sessions);

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/v1/monitor", drogon::Get);
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr &request,
                             const drogon::WebSocketConnectionPtr &connection) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr &connection,
                          std::string &&message,
                          const drogon::WebSocketMessageType &type) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) override;

  private:
    drogon::orm::DbClientPtr dbClient_;
    std::shared_ptr<labscope::auth::SessionStore> sessions_;
};

}  // namespace labscope::api
