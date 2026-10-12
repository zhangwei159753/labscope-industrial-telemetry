#pragma once

#include <string>
#include <string_view>

namespace labscope::auth
{

class PasswordHasher final
{
  public:
    static std::size_t minimumPasswordLengthForEnvironment(
        std::string_view environment) noexcept;
    static std::string hashPassword(std::string_view password);
    static bool verifyPassword(std::string_view encodedHash,
                               std::string_view password) noexcept;
    static std::string randomToken();
    static std::string hashToken(std::string_view token);
    static bool constantTimeEqual(std::string_view left,
                                  std::string_view right) noexcept;
};

}  // namespace labscope::auth
