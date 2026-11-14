/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

/**
 * @file Jwt.cpp
 * @brief Authentication with a JSON Web Token presented as the MQTT password.
 *
 * The broker needs no directory for this: the token carries a signature the extension can check by
 * itself, and the claims say who the client is and what it may be. A client that already holds a
 * token from the customer's identity provider connects without another round trip, and the broker
 * trusts the issuer exactly as far as the signature and the claims go.
 *
 * Three algorithms are accepted: HS256 with a shared secret, RS256 with an RSA public key, and
 * ES256 with a P-256 public key. The key is either configured directly or fetched from the issuer's
 * JWKS endpoint, which is what lets a provider rotate its signing key without the broker being
 * reconfigured.
 *
 * The password is where the token goes, because MQTT has no field of its own for one. A password
 * that is not three dot-separated base64url parts is somebody else's business: the extension
 * answers NOT_HANDLED and the chain carries on, so local accounts keep working beside it.
 *
 * Claim names are configurable, and a claim may be a dotted path ("realm_access.roles"), because
 * providers disagree about where they put groups. A group claim may be a string or an array.
 */

#include "JwtVerify.h"

#include "extension/XmqExtension.h"
#include <openssl/hmac.h>
#include <openssl/rand.h>

#ifndef _WIN32
#include <sys/socket.h>
#else
#ifdef max
#undef max
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------------------------

/// How long a fetched key set is trusted before it is fetched again. Long enough that a CONNECT
/// never waits for it, short enough that a provider's rotation is noticed without a restart; a
/// token naming a key the set does not hold refreshes it at once.
constexpr int keySetLifetimeSeconds = 300;

struct Mapping
{
    std::string claimValue;
    std::string group;
};

struct Config
{
    std::string          algorithm{"RS256"};
    std::string          secret;
    std::string          publicKeyPem;
    std::string          jwksUrl;
    std::string          jwksCaFile;
    std::string          issuer;
    std::string          audience;
    std::string          usernameClaim{"preferred_username"};
    std::string          groupsClaim{"groups"};
    std::string          prefix;
    std::vector<Mapping> mappings;
    int                  skewSeconds{30};
    int                  timeoutSeconds{5};
    int                  cacheSeconds{60};
    int                  negativeCacheSeconds{10};
    size_t               cacheEntries{4096};
};

/// "claim value => XMQ group" per line, the same shape the LDAP extension uses for directory
/// groups: the left side is what the token says, the right side is what Permissions already knows.
std::vector<Mapping> parseMappings(const std::string& text)
{
    std::vector<Mapping> mappings;
    std::istringstream   stream(text);
    std::string          line;
    while (std::getline(stream, line))
    {
        const auto separator = line.find("=>");
        if (separator == std::string::npos)
        {
            continue;
        }
        const auto trim = [](std::string_view value)
        {
            const auto first = value.find_first_not_of(" \t\r");
            const auto last = value.find_last_not_of(" \t\r");
            return first == std::string_view::npos
                       ? std::string{}
                       : std::string(value.substr(first, last - first + 1));
        };
        const auto claimValue = trim(std::string_view(line).substr(0, separator));
        const auto group = trim(std::string_view(line).substr(separator + 2));
        if (!claimValue.empty() && !group.empty())
        {
            mappings.push_back({claimValue, group});
        }
    }
    return mappings;
}

void validate(const Config& config)
{
    if (config.algorithm != "HS256" && config.algorithm != "RS256" && config.algorithm != "ES256")
        throw std::runtime_error("JWT algorithm must be HS256, RS256 or ES256");
    if (config.issuer.empty())
        throw std::runtime_error("JWT issuer is required: without it any token signed by these keys "
            "would be accepted from anywhere");
    if (config.usernameClaim.empty())
        throw std::runtime_error("JWT username_claim must not be empty");
    if (config.algorithm == "HS256")
    {
        if (config.secret.empty())
            throw std::runtime_error("JWT HS256 needs a shared secret");
        if (!config.jwksUrl.empty() || !config.publicKeyPem.empty())
            throw std::runtime_error("JWT HS256 uses the shared secret alone: clear public_key and jwks_url");
    }
    else
    {
        if (!config.secret.empty())
            throw std::runtime_error("JWT HS256 uses the shared secret alone: clear public_key and jwks_url");
        if (config.publicKeyPem.empty() && config.jwksUrl.empty())
            throw std::runtime_error("JWT RS256/ES256 needs either public_key or jwks_url");
        if (!config.publicKeyPem.empty() && !config.jwksUrl.empty())
            throw std::runtime_error("JWT takes its key from public_key or jwks_url, not both");
        if (!config.jwksUrl.empty() &&
            (!config.jwksUrl.starts_with("https://") && !config.jwksUrl.starts_with("http://")))
            throw std::runtime_error("JWT jwks_url must be an https:// URL");
    }
}

/// Plain HTTP is refused except on loopback, where it cannot be intercepted from another machine
/// and where a local test stand lives. Everything else has to be TLS with a verified certificate.
bool plainHttpAllowed(const std::string& url)
{
    if (!url.starts_with("http://"))
    {
        return true;
    }
    const auto hostStart = url.find("://") + 3;
    const auto hostEnd = url.find_first_of(":/", hostStart);
    const auto host = url.substr(hostStart, hostEnd == std::string::npos
                                                ? std::string::npos
                                                : hostEnd - hostStart);
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

/// Keep ordinary MQTT passwords in the rest of the authenticator chain. Actual token parsing,
/// base64url validation and signature verification are delegated to SPTK::JWT.
bool hasJwtShape(const std::string_view password)
{
    const auto first = password.find('.');
    const auto second = first == std::string_view::npos ? first : password.find('.', first + 1);
    return first != std::string_view::npos && first != 0 && second != std::string_view::npos &&
           second != first + 1 && password.find('.', second + 1) == std::string_view::npos;
}

// ---------------------------------------------------------------------------------------------
// The issuer's key set, fetched and kept for a while: a CONNECT must not wait for a key fetch, and
// a provider rotating its key must be noticed without a restart.
// ---------------------------------------------------------------------------------------------

struct KeySet
{
    std::vector<Jwk>  keys;
    Clock::time_point fetched;
};

/// A GET over TLS with the certificate verified against the system store or the configured CA, and
/// the host name checked against the certificate. Written out rather than pulled from a library so
/// that the extension needs nothing but OpenSSL and the ABI.
std::optional<std::string> httpsGet(const std::string& url, const std::string&      caFile,
                                    const int          timeoutSeconds, std::string& error)
{
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
    {
        error = "JWKS URL has no scheme";
        return std::nullopt;
    }
    const auto scheme = url.substr(0, schemeEnd);
    if (scheme != "https" && scheme != "http")
    {
        error = "JWKS URL must be https";
        return std::nullopt;
    }
    const auto authorityStart = schemeEnd + 3;
    const auto pathStart = url.find('/', authorityStart);
    const auto authority = url.substr(authorityStart, pathStart == std::string::npos
                                                          ? std::string::npos
                                                          : pathStart - authorityStart);
    const auto path = pathStart == std::string::npos ? std::string("/") : url.substr(pathStart);
    if (authority.empty())
    {
        error = "JWKS URL has no host";
        return std::nullopt;
    }
    auto host = authority;
    auto port = scheme == "https" ? std::string("443") : std::string("80");
    if (const auto colon = authority.rfind(':'); colon != std::string::npos &&
                                                 authority.find(']') == std::string::npos)
    {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }

    // Loopback plain HTTP is a documented exception; everything else goes through the TLS path
    // below with the certificate and the host name verified.
    if (scheme == "http")
    {
        BIO* plain = BIO_new_connect((host + ":" + port).c_str());
        if (plain == nullptr || BIO_do_connect(plain) != 1)
        {
            BIO_free_all(plain);
            error = "JWKS host did not answer: " + host + ":" + port;
            return std::nullopt;
        }
        const std::unique_ptr<BIO, decltype(&BIO_free_all)> bio(plain, BIO_free_all);
        const std::string                                   request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                                    "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
        if (BIO_write(bio.get(), request.data(), static_cast<int>(request.size())) !=
            static_cast<int>(request.size()))
        {
            error = "the JWKS request could not be sent";
            return std::nullopt;
        }
        std::string            response;
        std::array<char, 4096> buffer{};
        while (true)
        {
            const auto read = BIO_read(bio.get(), buffer.data(), static_cast<int>(buffer.size()));
            if (read <= 0)
            {
                break;
            }
            response.append(buffer.data(), static_cast<size_t>(read));
            if (response.size() > 4 * 1024 * 1024)
            {
                error = "the JWKS answer is unreasonably large";
                return std::nullopt;
            }
        }
        const auto headerEnd = response.find("\r\n\r\n");
        if (headerEnd == std::string::npos ||
            response.substr(0, response.find("\r\n")).find(" 200 ") == std::string::npos)
        {
            error = "the JWKS endpoint did not answer 200";
            return std::nullopt;
        }
        return response.substr(headerEnd + 4);
    }

    SSL_CTX* rawContext = SSL_CTX_new(TLS_client_method());
    if (rawContext == nullptr)
    {
        error = "TLS context could not be created";
        return std::nullopt;
    }
    const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(rawContext, SSL_CTX_free);
    SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    if (!caFile.empty())
    {
        if (SSL_CTX_load_verify_locations(context.get(), caFile.c_str(), nullptr) != 1)
        {
            error = "JWKS CA certificate could not be loaded: " + caFile;
            return std::nullopt;
        }
    }
    else if (SSL_CTX_set_default_verify_paths(context.get()) != 1)
    {
        error = "the system CA store could not be loaded";
        return std::nullopt;
    }

    BIO* rawBio = BIO_new_ssl_connect(context.get());
    if (rawBio == nullptr)
    {
        error = "TLS connection could not be created";
        return std::nullopt;
    }
    const std::unique_ptr<BIO, decltype(&BIO_free_all)> bio(rawBio, BIO_free_all);
    SSL*                                                ssl = nullptr;
    BIO_get_ssl(bio.get(), &ssl);
    if (ssl == nullptr)
    {
        error = "TLS connection has no SSL handle";
        return std::nullopt;
    }
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    const auto endpoint = host + ":" + port;
    BIO_set_conn_hostname(bio.get(), endpoint.c_str());
    if (BIO_do_connect(bio.get()) != 1)
    {
        error = "JWKS host did not answer: " + endpoint;
        return std::nullopt;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK)
    {
        error = "JWKS certificate did not verify: " +
                std::string(X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
        return std::nullopt;
    }

    // The timeout is applied to the socket after connecting: the handshake itself has no deadline,
    // which is a limitation worth knowing rather than a promise of one. Winsock wants a DWORD and
    // this block is POSIX-only, so a Windows build keeps the transport's own timeout.
#ifndef _WIN32
    const int descriptor = BIO_get_fd(bio.get(), nullptr);
    if (descriptor >= 0)
    {
        timeval limit{timeoutSeconds, 0};
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    }
#endif

    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                                "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    if (BIO_write(bio.get(), request.data(), static_cast<int>(request.size())) !=
        static_cast<int>(request.size()))
    {
        error = "the JWKS request could not be sent";
        return std::nullopt;
    }
    std::string            response;
    std::array<char, 4096> buffer{};
    while (true)
    {
        const auto read = BIO_read(bio.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (read <= 0)
        {
            break;
        }
        response.append(buffer.data(), static_cast<size_t>(read));
        if (response.size() > 4 * 1024 * 1024)
        {
            error = "the JWKS answer is unreasonably large";
            return std::nullopt;
        }
    }
    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        error = "the JWKS answer has no HTTP headers";
        return std::nullopt;
    }
    const auto status = response.substr(0, response.find("\r\n"));
    if (status.find(" 200 ") == std::string::npos)
    {
        error = "the JWKS endpoint answered: " + status;
        return std::nullopt;
    }
    return response.substr(headerEnd + 4);
}

class JwksCache
{
public:
    void configure(const int lifetimeSeconds)
    {
        const std::scoped_lock lock(m_mutex);
        m_lifetime = std::chrono::seconds(std::max(1, lifetimeSeconds));
        m_keys.reset();
    }

    void clear()
    {
        const std::scoped_lock lock(m_mutex);
        m_keys.reset();
    }

    /// The key for a token, refetched when the set has aged out or when a key id is not in it.
    /// `error` is filled when the set could not be obtained at all, which is an outage rather than
    /// a refusal.
    PkeyPtr key(const Config& config, const std::string& algorithm, const std::string& keyId,
                std::string&  error)
    {
        // A configured key is the whole set and has no id to match: the token's kid is not ours
        // to check against a key the operator pasted in.
        const auto wanted = config.publicKeyPem.empty() ? keyId : std::string{};
        {
            const std::scoped_lock lock(m_mutex);
            if (m_keys && Clock::now() - m_keys->fetched < m_lifetime)
            {
                if (auto found = select(*m_keys, algorithm, wanted); found)
                {
                    return found;
                }
            }
        }
        if (!refresh(config, error))
        {
            return {};
        }
        const std::scoped_lock lock(m_mutex);
        if (!m_keys)
        {
            error = "the JWKS answer holds no usable key";
            return {};
        }
        auto found = select(*m_keys, algorithm, wanted);
        if (!found)
        {
            error = "no key in the JWKS matches this token";
        }
        return found;
    }

private:
    static PkeyPtr select(const KeySet& set, const std::string& algorithm, const std::string& keyId)
    {
        for (const auto& jwk: set.keys)
        {
            if (!keyId.empty() && jwk.keyId != keyId)
            {
                continue;
            }
            if (!jwk.algorithm.empty() && jwk.algorithm != algorithm)
            {
                continue;
            }
            if (jwk.key)
            {
                return jwk.key;
            }
        }
        return {};
    }

    bool refresh(const Config& config, std::string& error)
    {
        if (!config.publicKeyPem.empty())
        {
            // A configured key needs no network: it is the whole set.
            const auto key = publicKeyFromPem(config.publicKeyPem);
            if (!key)
            {
                error = "the configured public key could not be read";
                return false;
            }
            const std::scoped_lock lock(m_mutex);
            m_keys = KeySet{{Jwk{"", config.algorithm, key}}, Clock::now()};
            return true;
        }
        std::string transportError;
        const auto  body = httpsGet(config.jwksUrl, config.jwksCaFile, config.timeoutSeconds,
                                   transportError);
        if (!body)
        {
            error = transportError;
            return false;
        }
        sptk::xdoc::Document document;
        try
        {
            document.load(sptk::Buffer(*body));
        }
        catch (const std::exception&)
        {
            error = "the JWKS answer is not valid JSON";
            return false;
        }
        if (document.root()->type() != sptk::xdoc::Node::Type::Object)
        {
            error = "the JWKS answer is not a JSON object";
            return false;
        }
        auto keys = keysFromJwks(document.root());
        if (keys.empty())
        {
            error = "the JWKS answer holds no signing keys";
            return false;
        }
        const std::scoped_lock lock(m_mutex);
        m_keys = KeySet{std::move(keys), Clock::now()};
        return true;
    }

    std::mutex            m_mutex;
    std::chrono::seconds  m_lifetime{300};
    std::optional<KeySet> m_keys;
};

// ---------------------------------------------------------------------------------------------
// Decisions, remembered for the same reason the LDAP extension remembers them: every CONNECT would
// otherwise repeat work whose answer cannot have changed in the last minute.
// ---------------------------------------------------------------------------------------------

struct CacheEntry
{
    Clock::time_point        expires;
    xmq_auth_decision        decision;
    std::vector<std::string> groups;
};

class DecisionCache
{
public:
    void configure(const int positives, const int negatives, const size_t maxEntries)
    {
        const std::scoped_lock lock(m_mutex);
        m_positive = std::chrono::seconds(std::max(0, positives));
        m_negative = std::chrono::seconds(std::max(0, negatives));
        m_maxEntries = std::max<size_t>(1, maxEntries);
        m_entries.clear();
        m_order.clear();
    }

    bool find(const std::string& key, xmq_auth_decision& decision, std::vector<std::string>& groups)
    {
        const std::scoped_lock lock(m_mutex);
        const auto             found = m_entries.find(key);
        if (found == m_entries.end())
        {
            return false;
        }
        if (found->second.expires <= Clock::now())
        {
            m_order.erase(found->second.order);
            m_entries.erase(found);
            return false;
        }
        m_order.splice(m_order.begin(), m_order, found->second.order);
        decision = found->second.decision;
        groups = found->second.groups;
        return true;
    }

    /// An outage is never stored: it must not become a remembered answer in either direction.
    void store(const std::string&              key, const xmq_auth_decision decision,
               const std::vector<std::string>& groups)
    {
        if (decision == XMQ_AUTH_SUBSYSTEM_ERROR)
        {
            return;
        }
        const auto lifetime = decision == XMQ_AUTH_ALLOW ? m_positive : m_negative;
        if (lifetime.count() == 0)
        {
            return;
        }
        const std::scoped_lock lock(m_mutex);
        const auto             now = Clock::now();
        for (auto entry = m_entries.begin(); entry != m_entries.end();)
        {
            if (entry->second.expires <= now)
            {
                m_order.erase(entry->second.order);
                entry = m_entries.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
        if (const auto existing = m_entries.find(key); existing != m_entries.end())
        {
            m_order.erase(existing->second.order);
            m_entries.erase(existing);
        }
        if (m_entries.size() >= m_maxEntries)
        {
            m_entries.erase(m_order.back());
            m_order.pop_back();
        }
        m_order.push_front(key);
        m_entries.emplace(key, Entry{now + lifetime, decision, groups, m_order.begin()});
    }

private:
    struct Entry
    {
        Clock::time_point                expires;
        xmq_auth_decision                decision;
        std::vector<std::string>         groups;
        std::list<std::string>::iterator order;
    };

    std::mutex                             m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::list<std::string>                 m_order;
    std::chrono::seconds                   m_positive{60};
    std::chrono::seconds                   m_negative{10};
    size_t                                 m_maxEntries{4096};
};

/// A decision is keyed by the presented identity and a keyed hash of the token: the cache never
/// holds the credential itself, a restarted broker shares no keys with the previous process, and a
/// token tried under another name misses instead of reusing the answer for the first name.
std::string decisionKey(const std::array<unsigned char, 32>& salt, const std::string_view username,
                        const std::string&                   token)
{
    if (std::all_of(salt.begin(), salt.end(), [](const auto byte) { return byte == 0; }))
    {
        return {};
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (HMAC(EVP_sha256(), salt.data(), static_cast<int>(salt.size()),
             reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest,
             &length) == nullptr)
    {
        return {};
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string           key(username);
    key += '\0';
    for (unsigned int index = 0; index < length; ++index)
    {
        key += hex[digest[index] >> 4];
        key += hex[digest[index] & 15];
    }
    return key;
}

// ---------------------------------------------------------------------------------------------
// Claims.
// ---------------------------------------------------------------------------------------------

using Claims = sptk::xdoc::SNode;

sptk::xdoc::SNode findClaim(const sptk::xdoc::SNode& root, const std::string_view path)
{
    auto node = root;
    size_t start = 0;
    while (node && start <= path.size())
    {
        const auto end = path.find('.', start);
        const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start
                                                                           : end - start);
        if (node->type() != sptk::xdoc::Node::Type::Object)
            return {};
        node = node->findFirst(sptk::String(part.data(), part.size()),
                               sptk::xdoc::SearchMode::ImmediateChild);
        if (end == std::string_view::npos)
            return node;
        start = end + 1;
    }
    return {};
}

std::string claimString(const Claims& claims, const std::string_view path)
{
    const auto node = findClaim(claims, path);
    return node && node->type() == sptk::xdoc::Node::Type::Text
               ? std::string(node->getString())
               : std::string{};
}

bool claimNumber(const Claims& claims, const std::string_view path, int64_t& result)
{
    const auto node = findClaim(claims, path);
    if (!node || node->type() != sptk::xdoc::Node::Type::Number)
        return false;
    result = static_cast<int64_t>(node->getNumber());
    return true;
}

std::vector<std::string> claimStrings(const Claims& claims, const std::string_view path)
{
    const auto node = findClaim(claims, path);
    if (!node)
        return {};
    if (node->type() == sptk::xdoc::Node::Type::Text)
        return {std::string(node->getString())};
    std::vector<std::string> values;
    if (node->type() == sptk::xdoc::Node::Type::Array)
    {
        for (const auto& item : node->nodes())
        {
            if (item->type() == sptk::xdoc::Node::Type::Text)
                values.emplace_back(item->getString());
        }
    }
    return values;
}

bool claimAudienceMatches(const Claims& claims, const std::string& expected)
{
    const auto node = findClaim(claims, "aud");
    if (!node)
        return false;
    if (node->type() == sptk::xdoc::Node::Type::Text)
        return node->getString() == expected;
    if (node->type() == sptk::xdoc::Node::Type::Array)
    {
        for (const auto& item : node->nodes())
        {
            if (item->type() == sptk::xdoc::Node::Type::Text && item->getString() == expected)
                return true;
        }
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The extension.
// ---------------------------------------------------------------------------------------------

namespace {

std::string publicKeyPem(EVP_PKEY* key)
{
    BIO* rawBio = BIO_new(BIO_s_mem());
    if (!rawBio) throw std::runtime_error("SPTK JWT key serialization failed");
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(rawBio, BIO_free);
    if (PEM_write_bio_PUBKEY(bio.get(), key) != 1)
        throw std::runtime_error("SPTK JWT key serialization failed");
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length <= 0 || !data)
        throw std::runtime_error("SPTK JWT key serialization failed");
    return {data, static_cast<size_t>(length)};
}

class JwtAuthenticator final : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    static constexpr bool NativeGroups = true;

    bool start() override
    {
        if (RAND_bytes(m_salt.data(), static_cast<int>(m_salt.size())) != 1)
        {
            m_salt.fill(0);
            return false;
        }
        return reload();
    }

    bool reload() override
    {
        try
        {
            auto config = std::make_shared<Config>();
            config->algorithm = setting("algorithm", "RS256");
            config->secret = setting("secret");
            config->publicKeyPem = setting("public_key");
            config->jwksUrl = setting("jwks_url");
            config->jwksCaFile = setting("jwks_ca_file");
            config->issuer = setting("issuer");
            config->audience = setting("audience");
            config->usernameClaim = setting("username_claim", "preferred_username");
            config->groupsClaim = setting("groups_claim", "groups");
            config->prefix = setting("username_prefix", "jwt/");
            config->mappings = parseMappings(setting("group_mappings"));
            config->skewSeconds = std::clamp(number("clock_skew_seconds", "30"), 0, 300);
            config->timeoutSeconds = std::clamp(number("timeout_seconds", "5"), 1, 60);
            config->cacheSeconds = std::clamp(number("cache_ttl_seconds", "60"), 0, 3600);
            config->negativeCacheSeconds =
                std::clamp(number("negative_cache_ttl_seconds", "10"), 0, 3600);
            config->cacheEntries = static_cast<size_t>(
                std::clamp(number("cache_max_entries", "4096"), 1, 1000000));
            validate(*config);
            if (!config->jwksUrl.empty() && !plainHttpAllowed(config->jwksUrl))
                throw std::runtime_error("JWT jwks_url must use https outside loopback");
            if (!config->jwksUrl.empty() && config->publicKeyPem.empty())
            {
                // One fetch at load time, so a wrong URL or CA is reported where it is configured
                // rather than on the first connection.
                std::string error;
                m_jwks.configure(keySetLifetimeSeconds);
                if (!m_jwks.key(*config, config->algorithm, "", error))
                    throw std::runtime_error("JWT key set is not available: " + error);
            }
            else
            {
                m_jwks.configure(keySetLifetimeSeconds);
            }
            const std::scoped_lock lock(m_mutex);
            m_config = std::move(config);
            m_cache.configure(m_config->cacheSeconds, m_config->negativeCacheSeconds,
                              m_config->cacheEntries);
            return true;
        }
        catch (const std::exception& error)
        {
            log(XMQ_LOG_ERROR, error.what());
            return false;
        }
    }

    xmq_auth_decision authenticate(const xmq_auth_request& request) override
    {
        std::vector<std::string> ignored;
        return authenticateWithGroups(request, ignored);
    }

    xmq_auth_decision authenticateWithGroups(const xmq_auth_request&   request,
                                             std::vector<std::string>& groups) override
    {
        groups.clear();
        std::shared_ptr<const Config> config;
        {
            const std::scoped_lock lock(m_mutex);
            config = m_config;
        }
        if (!config)
        {
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }
        if (request.password.data == nullptr || request.password.length == 0)
        {
            return XMQ_AUTH_NOT_HANDLED;
        }
        const std::string tokenText(request.password.data, request.password.length);
        if (!hasJwtShape(tokenText))
        {
            return XMQ_AUTH_NOT_HANDLED;
        }
        const std::string_view presentedName(request.username.data ? request.username.data : "",
                                             request.username.length);
        const auto key = decisionKey(m_salt, presentedName, tokenText);
        if (!key.empty())
        {
            xmq_auth_decision        cached{};
            std::vector<std::string> cachedGroups;
            if (m_cache.find(key, cached, cachedGroups))
            {
                groups = std::move(cachedGroups);
                return cached;
            }
        }

        sptk::JWT decodedToken;
        sptk::xdoc::Document decodedHeader;
        try
        {
            // Obtain the protected header's kid before choosing a JWKS key. Signature verification
            // happens in a second decode after that key has been selected.
            decodedToken.decode(tokenText.c_str(), "", &decodedHeader);
        }
        catch (const std::exception&)
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }
        const auto algorithm = claimString(decodedHeader.root(), "alg");
        // Exactly the configured algorithm: accepting whatever the token asks for is how an
        // attacker turns RS256 into HS256 and signs with the public key as the secret.
        if (algorithm != config->algorithm)
        {
            log(XMQ_LOG_WARNING, "rejected a token that asks for " + algorithm + ", not " +
                                 config->algorithm);
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }

        std::string error;
        PkeyPtr     signingKey;
        std::string verificationKey;
        if (config->algorithm == "HS256")
        {
            verificationKey = config->secret;
        }
        else
        {
            const auto keyId = claimString(decodedHeader.root(), "kid");
            signingKey = m_jwks.key(*config, config->algorithm, keyId, error);
            if (!signingKey)
            {
                // A missing/unavailable JWKS is a subsystem failure, not a bad password.
                log(XMQ_LOG_ERROR, "JWT key set: " + error);
                return XMQ_AUTH_SUBSYSTEM_ERROR;
            }
            try
            {
                verificationKey = publicKeyPem(signingKey.get());
            }
            catch (const std::exception& exception)
            {
                log(XMQ_LOG_ERROR, exception.what());
                return XMQ_AUTH_SUBSYSTEM_ERROR;
            }
        }
        Claims claims;
        try
        {
            const auto expectedAlgorithm = sptk::JWT::str_alg(config->algorithm.c_str());
            decodedToken.decode(tokenText.c_str(), sptk::String(verificationKey), expectedAlgorithm);
            claims = decodedToken.grants.root();
        }
        catch (const std::exception&)
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }

        const auto  now = std::time(nullptr);
        const auto  skew = static_cast<int64_t>(config->skewSeconds);
        int64_t expiry = 0;
        if (!claimNumber(claims, "exp", expiry))
        {
            // A token with no expiry is a password that never changes.
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }
        if (expiry + skew <= now)
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }
        int64_t notBefore = 0;
        if (claimNumber(claims, "nbf", notBefore) && notBefore > now + skew)
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }
        if (claimString(claims, "iss") != config->issuer)
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }
        if (!config->audience.empty() && !claimAudienceMatches(claims, config->audience))
        {
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }

        const auto identity = claimString(claims, config->usernameClaim);
        if (identity.empty())
        {
            log(XMQ_LOG_WARNING, "rejected a token with no " + config->usernameClaim + " claim");
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }

        // The ABI lets an authenticator hand over groups, not a name: the session keeps the
        // username the client sent. So the token has to agree with it, or a token issued for one
        // identity could connect as another.
        if (presentedName != config->prefix + identity)
        {
            log(XMQ_LOG_WARNING, "rejected a token for " + identity + " presented as " +
                                 std::string(presentedName));
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_DENY, {});
            return XMQ_AUTH_DENY;
        }

        const auto claimedGroups = claimStrings(claims, config->groupsClaim);
        if (config->mappings.empty())
        {
            groups = claimedGroups;
        }
        else
        {
            for (const auto& mapping: config->mappings)
            {
                if (std::find(claimedGroups.begin(), claimedGroups.end(), mapping.claimValue) ==
                    claimedGroups.end())
                {
                    continue;
                }
                if (std::find(groups.begin(), groups.end(), mapping.group) == groups.end())
                {
                    groups.push_back(mapping.group);
                }
            }
            if (groups.empty())
            {
                log(XMQ_LOG_WARNING,
                    "token for " + identity + " names no group that is mapped, so it gets no access");
            }
        }
        if (!key.empty()) m_cache.store(key, XMQ_AUTH_ALLOW, groups);
        return XMQ_AUTH_ALLOW;
    }

private:
    int number(const char* name, const char* fallback) const
    {
        const auto text = setting(name, fallback);
        int        value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
            throw std::runtime_error(std::string("JWT ") + name + " must be an integer");
        return value;
    }

    mutable std::mutex            m_mutex;
    std::shared_ptr<const Config> m_config;
    DecisionCache                 m_cache;
    JwksCache                     m_jwks;
    std::array<unsigned char, 32> m_salt{};
};
} // namespace

const xmq_setting jwtSettings[]{
    {.name = "algorithm", .label = "Signature algorithm", .description = "HS256 verifies a shared secret; RS256 an RSA public key; ES256 a P-256 public key. Only the configured algorithm is accepted, so a token cannot choose its own", .type = XMQ_SETTING_CHOICE, .default_value = "RS256", .choices = "RS256,ES256,HS256", .required = 0},
    {.name = "secret", .label = "Shared secret", .description = "The HMAC secret for HS256, as the identity provider has it. Not used by RS256 or ES256", .type = XMQ_SETTING_SECRET, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "public_key", .label = "Public key (PEM)", .description = "PEM public key for RS256 or ES256. Leave empty to take the keys from jwks_url instead", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "jwks_url", .label = "JWKS URL", .description = "The issuer's JSON Web Key Set, fetched over verified TLS and refreshed when a token names a key it does not hold", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "jwks_ca_file", .label = "JWKS CA certificate", .description = "The CA that signed the JWKS server's certificate - not the token signing key. Empty uses the system trust store", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "issuer", .label = "Issuer", .description = "The exact iss claim every accepted token must carry, for example https://keycloak.example.org/realms/xmq", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 1},
    {.name = "audience", .label = "Audience", .description = "The aud claim a token must name. Empty accepts any audience, which is weaker: leave it set wherever the provider issues one", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "username_claim", .label = "Username claim", .description = "Claim that carries the identity; a dotted path is allowed, for example preferred_username", .type = XMQ_SETTING_STRING, .default_value = "preferred_username", .choices = nullptr, .required = 0},
    {.name = "groups_claim", .label = "Groups claim", .description = "Claim that carries group or role names, as a string or an array; a dotted path is allowed, for example realm_access.roles", .type = XMQ_SETTING_STRING, .default_value = "groups", .choices = nullptr, .required = 0},
    {.name = "username_prefix", .label = "Username prefix", .description = "Prefix the identity from the token is presented with, so a token cannot stand in for a local account of the same name. The client sends this as its MQTT username and the token as its password; empty requires the claim to be sent as it is", .type = XMQ_SETTING_STRING, .default_value = "jwt/", .choices = nullptr, .required = 0},
    {.name = "group_mappings", .label = "Claims to XMQ groups", .description = "One 'claim value => existing XMQ group' per line. Empty passes the claim values through as group names", .type = XMQ_SETTING_STRING, .default_value = nullptr, .choices = nullptr, .required = 0},
    {.name = "clock_skew_seconds", .label = "Clock skew (seconds)", .description = "How far a token's exp and nbf may disagree with this host's clock", .type = XMQ_SETTING_INTEGER, .default_value = "30", .choices = nullptr, .required = 0},
    {.name = "timeout_seconds", .label = "Operation timeout (seconds)", .description = "Timeout for fetching the key set", .type = XMQ_SETTING_INTEGER, .default_value = "5", .choices = nullptr, .required = 0},
    {.name = "cache_ttl_seconds", .label = "Cache verified tokens (seconds)", .description = "How long a verified token and its groups are remembered. 0 disables the cache", .type = XMQ_SETTING_INTEGER, .default_value = "60", .choices = nullptr, .required = 0},
    {.name = "negative_cache_ttl_seconds", .label = "Cache refused tokens (seconds)", .description = "How long a refused token is remembered; 0 disables it", .type = XMQ_SETTING_INTEGER, .default_value = "10", .choices = nullptr, .required = 0},
    {.name = "cache_max_entries", .label = "Cached tokens", .description = "Upper bound on remembered tokens, least recently used evicted first", .type = XMQ_SETTING_INTEGER, .default_value = "4096", .choices = nullptr, .required = 0}
};

XMQ_DEFINE_EXTENSION_DESCRIBED(JwtAuthenticator, "jwt", "0.1", XMQ_CAP_AUTHENTICATOR,
                               "JSON Web Token authentication (HS256, RS256, ES256) with claims mapped to XMQ groups",
                               jwtSettings)
