#pragma once

#include "SessionStore.h"

#include <drogon/HttpFilter.h>

#include <memory>

namespace labscope::auth
{

inline constexpr char kSessionCookie[] = "labscope_session";

class SessionFilter final : public drogon::HttpFilter<SessionFilter, false>
{
  public:
    explicit SessionFilter(std::shared_ptr<SessionStore> sessions);
    void doFilter(const drogon::HttpRequestPtr &request,
                  drogon::FilterCallback &&filterCallback,
                  drogon::FilterChainCallback &&chainCallback) override;

  private:
    std::shared_ptr<SessionStore> sessions_;
};

}  // namespace labscope::auth
