#include "PasswordHasher.h"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <stdexcept>

namespace labscope::auth
{
namespace
{
void ensureSodium()
{
    static const int initialized = sodium_init();
    if (initialized < 0)
        throw std::runtime_error("libsodium initialization failed");
}

std::string hexEncode(const unsigned char *data, std::size_t size)
{
    std::string output(size * 2 + 1, '\0');
    sodium_bin2hex(output.data(), output.size(), data, size);
    output.resize(size * 2);
    return output;
}
}  // namespace

std::size_t PasswordHasher::minimumPasswordLengthForEnvironment(
    std::string_view environment) noexcept
{
    return environment == "development" ? 6 : 12;
}

std::string PasswordHasher::hashPassword(std::string_view password)
{
    const char *environment = std::getenv("LABSCOPE_ENV");
    const std::string_view environmentName = environment ? environment : "";
    const auto minimumLength = minimumPasswordLengthForEnvironment(environmentName);
    if (password.size() < minimumLength || password.size() > 1024)
        throw std::invalid_argument("password length is invalid for the configured environment");
    ensureSodium();
    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(encoded.data(), password.data(), password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE,
                              crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw std::runtime_error("password hashing failed due to insufficient memory");
    return std::string(encoded.data());
}

bool PasswordHasher::verifyPassword(std::string_view encodedHash,
                                    std::string_view password) noexcept
{
    if (encodedHash.empty() || encodedHash.size() >= crypto_pwhash_STRBYTES ||
        password.size() > 1024)
        return false;
    try
    {
        ensureSodium();
        const std::string hash(encodedHash);
        return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
    }
    catch (...)
    {
        return false;
    }
}

std::string PasswordHasher::randomToken()
{
    ensureSodium();
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    return hexEncode(bytes.data(), bytes.size());
}

std::string PasswordHasher::hashToken(std::string_view token)
{
    ensureSodium();
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256(digest.data(),
                           reinterpret_cast<const unsigned char *>(token.data()),
                           token.size()) != 0)
        throw std::runtime_error("session token hashing failed");
    return hexEncode(digest.data(), digest.size());
}

bool PasswordHasher::constantTimeEqual(std::string_view left,
                                       std::string_view right) noexcept
{
    if (left.size() != right.size())
        return false;
    try
    {
        ensureSodium();
        return sodium_memcmp(left.data(), right.data(), left.size()) == 0;
    }
    catch (...)
    {
        return false;
    }
}

}  // namespace labscope::auth
