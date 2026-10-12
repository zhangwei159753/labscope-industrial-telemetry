#include "auth/PasswordHasher.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        require(labscope::auth::PasswordHasher::minimumPasswordLengthForEnvironment(
                    "development") == 6,
                "development passwords may use the local demo minimum");
        require(labscope::auth::PasswordHasher::minimumPasswordLengthForEnvironment(
                    "production") == 12,
                "production passwords must retain the stronger minimum");
        const std::string password = "correct-horse-battery-42";
        const auto encoded = labscope::auth::PasswordHasher::hashPassword(password);
        require(encoded.rfind("$argon2id$", 0) == 0, "password hash must use Argon2id");
        require(labscope::auth::PasswordHasher::verifyPassword(encoded, password),
                "valid password must verify");
        require(!labscope::auth::PasswordHasher::verifyPassword(encoded, "incorrect-password"),
                "wrong password must fail");
        require(!labscope::auth::PasswordHasher::verifyPassword("invalid", password),
                "malformed hash must fail closed");

        const auto token = labscope::auth::PasswordHasher::randomToken();
        require(token.size() == 64, "session token must be 256 bits encoded as hex");
        require(labscope::auth::PasswordHasher::hashToken(token).size() == 64,
                "stored session hash must be SHA-256 hex");
        require(labscope::auth::PasswordHasher::constantTimeEqual(token, token),
                "constant time comparison accepts equal values");
        require(!labscope::auth::PasswordHasher::constantTimeEqual(token, token + "x"),
                "constant time comparison rejects different lengths");
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "password hashing contracts passed\n";
    return 0;
}
