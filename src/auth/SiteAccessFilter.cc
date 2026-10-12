#include "SiteAccessFilter.h"

#include "Authorization.h"

#include <drogon/drogon.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace labscope::auth
{
namespace
{
void reject(const drogon::FilterCallback &callback,
            drogon::HttpStatusCode status,
            const char *message)
{
    Json::Value body(Json::objectValue);
    body["error"] = message;
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    callback(response);
}
}

SiteAccessFilter::SiteAccessFilter(drogon::orm::DbClientPtr dbClient)
    : dbClient_(std::move(dbClient))
{
    if (!dbClient_)
        throw std::invalid_argument("site access filter requires a database client");
}

void SiteAccessFilter::doFilter(const drogon::HttpRequestPtr &request,
                                drogon::FilterCallback &&filterCallback,
                                drogon::FilterChainCallback &&chainCallback)
{
    const auto *context = Authorization::context(request);
    if (!context)
    {
        reject(filterCallback, drogon::k401Unauthorized, "authentication required");
        return;
    }
    if (context->role == Role::Admin)
    {
        chainCallback();
        return;
    }

    const auto siteId = request->getParameter("site_id");
    if (siteId.empty())
    {
        reject(filterCallback, drogon::k400BadRequest, "site_id is required");
        return;
    }
    auto filterCallbackPtr = std::make_shared<drogon::FilterCallback>(
        std::move(filterCallback));
    auto chainCallbackPtr = std::make_shared<drogon::FilterChainCallback>(
        std::move(chainCallback));
    dbClient_->execSqlAsync(
        "SELECT EXISTS (SELECT 1 FROM web_user_sites "
        "WHERE user_id = $1 AND site_id = $2)",
        [filterCallbackPtr, chainCallbackPtr](const drogon::orm::Result &rows) mutable {
            if (rows.empty() || !rows[0][0].as<bool>())
            {
                reject(*filterCallbackPtr, drogon::k403Forbidden, "site access denied");
                return;
            }
            (*chainCallbackPtr)();
        },
        [filterCallbackPtr](
            const drogon::orm::DrogonDbException &) mutable {
            reject(*filterCallbackPtr, drogon::k503ServiceUnavailable,
                   "site access service unavailable");
        },
        context->userId,
        siteId);
}

}  // namespace labscope::auth
