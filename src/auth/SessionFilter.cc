#include "SessionFilter.h"

#include "PasswordHasher.h"

#include <drogon/drogon.h>

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
}  // namespace

SessionFilter::SessionFilter(std::shared_ptr<SessionStore> sessions)
    : sessions_(std::move(sessions))
{
    if (!sessions_)
        throw std::invalid_argument("session filter requires a session store");
}

void SessionFilter::doFilter(const drogon::HttpRequestPtr &request,
                             drogon::FilterCallback &&filterCallback,
                             drogon::FilterChainCallback &&chainCallback)
{
    const auto sessionToken = request->getCookie(kSessionCookie);
    if (sessionToken.empty())
    {
        reject(filterCallback, drogon::k401Unauthorized, "authentication required");
        return;
    }

    sessions_->resolve(sessionToken,
        [request,
         filterCallback = std::move(filterCallback),
         chainCallback = std::move(chainCallback)](
            std::optional<AuthContext> context, std::string error) mutable {
            if (!error.empty())
            {
                reject(filterCallback, drogon::k503ServiceUnavailable,
                       "authentication service unavailable");
                return;
            }
            if (!context)
            {
                reject(filterCallback, drogon::k401Unauthorized,
                       "session expired or revoked");
                return;
            }

            const auto method = request->method();
            const bool mutating = method != drogon::Get && method != drogon::Head &&
                                  method != drogon::Options;
            if (mutating && !PasswordHasher::constantTimeEqual(
                                request->getHeader("X-CSRF-Token"), context->csrfToken))
            {
                reject(filterCallback, drogon::k403Forbidden, "invalid CSRF token");
                return;
            }
            request->attributes()->insert("labscope.auth", std::move(*context));
            chainCallback();
        });
}

}  // namespace labscope::auth
