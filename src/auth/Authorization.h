#pragma once

#include <drogon/HttpRequest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace labscope::auth
{

enum class Role : uint8_t { Viewer = 0, Operator = 1, Admin = 2 };

struct AuthContext
{
    int64_t userId{0};
    std::string username;
    Role role{Role::Viewer};
    std::string csrfToken;
};

std::optional<Role> parseRole(const std::string &value);
const char *roleName(Role role) noexcept;

class Authorization final
{
  public:
    static bool allows(std::optional<Role> role, Role minimumRole) noexcept;
    static bool requireRole(const drogon::HttpRequestPtr &request,
                            Role minimumRole) noexcept;
    static const AuthContext *context(const drogon::HttpRequestPtr &request) noexcept;
};

}  // namespace labscope::auth
