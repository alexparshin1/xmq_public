#pragma once

#include <sptk5/net/URL.h>
#include <sptk5/Exception.h>
#include <string>

namespace xmq {
inline sptk::URL redisConfiguredUrl(std::string uri)
{
    if (uri.rfind("redis://:", 0) == 0) uri.insert(8, "default");
    return sptk::URL(uri);
}
inline std::string encodeRedisCredential(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c: value)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

inline std::string decodeRedisCredential(const std::string& value)
{
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '%')
        {
            if (i + 2 >= value.size() || digit(value[i + 1]) < 0 || digit(value[i + 2]) < 0)
            {
                throw sptk::Exception("Invalid percent encoding in a Redis credential.");
            }
            result += static_cast<char>((digit(value[i + 1]) << 4) | digit(value[i + 2]));
            i += 2;
        }
        else result += value[i];
    }
    return result;
}

// Apply once to a configured URI, before passing it to RedisConnect. Its own
// getRedisUrl() already contains decoded credentials and must not be decoded again.
inline sptk::URL redisConnectionUrl(const sptk::URL& configured)
{
    auto username = decodeRedisCredential(configured.username());
    const auto password = decodeRedisCredential(configured.password());
    if (!username.empty() && password.empty())
    {
        throw sptk::Exception("Enter a Redis password with the username, or leave both credentials empty.");
    }
    if (username.empty() && !password.empty()) username = "default";
    const auto& [host, port] = configured.hostAndPort();
    sptk::URL result(configured.protocol(), host, port, username, password, configured.path());
    result.params() = configured.params();
    return result;
}

inline sptk::URL redisConnectionUrl(const std::string& configured)
{
    return redisConnectionUrl(redisConfiguredUrl(configured));
}
}
