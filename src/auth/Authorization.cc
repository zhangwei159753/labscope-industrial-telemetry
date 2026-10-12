#include "Authorization.h"

namespace labscope::auth
{

std::optional<Role> parseRole(const std::string &value)
{
    if (value == "admin") return Role::Admin;
    if (value == "operator") return Role::Operator;
    if (value == "viewer") return Role::Viewer;
    return std::nullopt;
}

const char *roleName(Role role) noexcept
{
    switch (role)
    {
        case Role::Admin: return "admin";
        case Role::Operator: return "operator";
        case Role::Viewer: return "viewer";
    }
    return "viewer";
}

const AuthContext *Authorization::context(const drogon::HttpRequestPtr &request) noexcept
{
    if (!request || !request->attributes())
        return nullptr;
    const auto &context = request->attributes()->get<AuthContext>("labscope.auth");
    return context.userId > 0 ? &context : nullptr;
}

bool Authorization::allows(std::optional<Role> role, Role minimumRole) noexcept
{
    return role && static_cast<uint8_t>(*role) >= static_cast<uint8_t>(minimumRole);
}

bool Authorization::requireRole(const drogon::HttpRequestPtr &request,
                                Role minimumRole) noexcept
{
    const auto *user = context(request);
    return user && allows(user->role, minimumRole);
}

}  // namespace labscope::auth
