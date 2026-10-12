#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/orm/DbClient.h>

namespace labscope::auth
{

class SiteAccessFilter final : public drogon::HttpFilter<SiteAccessFilter, false>
{
  public:
    explicit SiteAccessFilter(drogon::orm::DbClientPtr dbClient);
    void doFilter(const drogon::HttpRequestPtr &request,
                  drogon::FilterCallback &&filterCallback,
                  drogon::FilterChainCallback &&chainCallback) override;

  private:
    drogon::orm::DbClientPtr dbClient_;
};

}  // namespace labscope::auth
