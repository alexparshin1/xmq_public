/* Copyright (c) 2026 Alexey Parshin. MPL-2.0. */
/**
 * HTTP authentication only: the remote service returns identity groups, never topic ACLs.
 * SPTK owns JSON, URL, HTTP and verified TLS. Outages never become cached credential refusals.
 */
#include "extension/XmqExtension.h"
#include <sptk5/net/HttpConnect.h>
#include <sptk5/net/SSLSocket.h>
#include <sptk5/net/URL.h>
#include <sptk5/xdoc/Document.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

std::string text(xmq_str value)
{
    if (!value.data && value.length) throw std::runtime_error("Invalid credential buffer");
    return value.data ? std::string(value.data, value.length) : std::string{};
}

struct HttpConfig
{
    std::string endpoint, caFile, bearer, prefix;
    int timeout{5}, positive{60}, negative{10}, maxEntries{4096}, maxInflight{16};
    std::array<unsigned char, 32> salt{};
    struct Entry {
        Clock::time_point expires;
        xmq_auth_decision decision;
        std::vector<std::string> groups;
        std::list<std::string>::iterator order;
    };
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::list<std::string> order;
};

struct HttpReply
{
    xmq_auth_decision decision{XMQ_AUTH_SUBSYSTEM_ERROR};
    std::vector<std::string> groups;
};

HttpReply parseReply(int status, const sptk::Buffer& body)
{
    if (status == 401 || status == 403) return {XMQ_AUTH_DENY, {}};
    if (status != 200) return {};
    const std::string content(body.c_str(), body.bytes());
    const auto first = content.find_first_not_of(" \r\n\t");
    if (first == std::string::npos || content[first] != '{') return {};
    sptk::xdoc::Document document;
    document.load(body);
    const auto root = document.root();
    if (root->type() != sptk::xdoc::Node::Type::Object) return {};
    const auto decision = root->findFirst("decision", sptk::xdoc::SearchMode::ImmediateChild);
    const auto value = decision ? decision->textValue() : std::nullopt;
    if (!value) return {};
    if (*value == "deny") return {XMQ_AUTH_DENY, {}};
    if (*value == "not_handled") return {XMQ_AUTH_NOT_HANDLED, {}};
    if (*value != "allow") return {};
    HttpReply reply{XMQ_AUTH_ALLOW, {}};
    const auto groups = root->findFirst("groups", sptk::xdoc::SearchMode::ImmediateChild);
    if (!groups) return reply; // empty membership is valid, but grants no MQTT topic access.
    if (groups->type() != sptk::xdoc::Node::Type::Array || groups->nodes().size() > 256) return {};
    for (const auto& group : groups->nodes())
    {
        const auto name = group->textValue();
        if (!name || name->empty() || name->size() > 256 || name->find('\0') != std::string::npos)
            return {};
        if (std::find(reply.groups.begin(), reply.groups.end(), *name) == reply.groups.end())
            reply.groups.emplace_back(*name);
    }
    return reply;
}

class HttpAuthenticator : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    static constexpr bool NativeGroups = true;
    bool start() override { return reload(); }
    bool reload() override
    {
        try
        {
            auto config = std::make_shared<HttpConfig>();
            config->endpoint = setting("url");
            config->caFile = setting("ca_file");
            config->bearer = setting("bearer_token");
            config->prefix = setting("username_prefix", "http/");
            config->timeout = number("timeout_seconds", "5", 1, 60);
            config->positive = number("cache_ttl_seconds", "60", 0, 3600);
            config->negative = number("negative_cache_ttl_seconds", "10", 0, 3600);
            config->maxEntries = number("cache_max_entries", "4096", 1, 1000000);
            config->maxInflight = number("max_inflight", "16", 1, 256);
            const sptk::URL url(config->endpoint);
            const auto [host, port] = url.hostAndPort();
            if (host.empty() || host.find(':') != std::string::npos ||
                !url.username().empty() || !url.password().empty())
                throw std::runtime_error("HTTP auth needs a DNS/IPv4 endpoint without URL credentials");
            if (url.protocol() != "https" &&
                !(url.protocol() == "http" && (host == "localhost" || host == "127.0.0.1")))
                throw std::runtime_error("HTTP auth requires HTTPS outside loopback");
            if (config->bearer.find_first_of("\r\n") != std::string::npos ||
                config->bearer.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid HTTP auth bearer header");
            if (RAND_bytes(config->salt.data(), config->salt.size()) != 1)
                throw std::runtime_error("HTTP auth cache entropy unavailable");
            const std::scoped_lock lock(m_mutex);
            m_config = std::move(config); // publish atomically; old in-flight calls keep old cache.
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
        std::vector<std::string> groups;
        return authenticateWithGroups(request, groups);
    }

    xmq_auth_decision authenticateWithGroups(const xmq_auth_request& request,
                                             std::vector<std::string>& groups) override
    {
        groups.clear();
        std::shared_ptr<HttpConfig> config;
        {
            const std::scoped_lock lock(m_mutex);
            config = m_config;
        }
        if (!config) return XMQ_AUTH_SUBSYSTEM_ERROR;
        try
        {
            if (request.username.length > 4096 || request.password.length > 65536 ||
                request.client_id.length > 4096 || request.remote_address.length > 4096)
                return XMQ_AUTH_DENY;
            const auto username = text(request.username);
            if (username.empty() || !username.starts_with(config->prefix)) return XMQ_AUTH_NOT_HANDLED;
            if (username.size() == config->prefix.size()) return XMQ_AUTH_DENY;
            sptk::xdoc::Document document;
            document.root()->set("username", sptk::String(username.substr(config->prefix.size())));
            document.root()->set("password", sptk::String(text(request.password)));
            document.root()->set("client_id", sptk::String(text(request.client_id)));
            document.root()->set("remote_address", sptk::String(text(request.remote_address)));
            document.root()->set("protocol_version", int(request.protocol_version));
            document.root()->set("encrypted", bool(request.encrypted));
            sptk::Buffer payload;
            document.exportTo(sptk::xdoc::DataFormat::JSON, payload);
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (!HMAC(EVP_sha256(), config->salt.data(), config->salt.size(),
                      payload.data(), payload.bytes(), digest, &length))
                return XMQ_AUTH_SUBSYSTEM_ERROR;
            const std::string key(reinterpret_cast<char*>(digest), length);
            {
                const std::scoped_lock lock(config->mutex);
                const auto entry = config->entries.find(key);
                if (entry != config->entries.end())
                {
                    if (entry->second.expires > Clock::now())
                    {
                        config->order.splice(config->order.begin(), config->order, entry->second.order);
                        groups = entry->second.groups;
                        return entry->second.decision;
                    }
                    config->order.erase(entry->second.order);
                    config->entries.erase(entry);
                }
            }
            auto inflight = m_inflight.load();
            do {
                if (inflight >= config->maxInflight) return XMQ_AUTH_SUBSYSTEM_ERROR;
            } while (!m_inflight.compare_exchange_weak(inflight, inflight + 1));
            struct Release {
                std::atomic<int>& count;
                ~Release() { --count; }
            } release{m_inflight};
            auto reply = fetch(*config, payload);
            if (reply.decision == XMQ_AUTH_ALLOW || reply.decision == XMQ_AUTH_DENY)
            {
                const auto ttl = reply.decision == XMQ_AUTH_ALLOW ? config->positive : config->negative;
                if (ttl)
                {
                    const std::scoped_lock lock(config->mutex);
                    if (const auto old = config->entries.find(key); old != config->entries.end())
                    {
                        config->order.erase(old->second.order);
                        config->entries.erase(old);
                    }
                    if (config->entries.size() >= size_t(config->maxEntries))
                    {
                        config->entries.erase(config->order.back());
                        config->order.pop_back();
                    }
                    config->order.push_front(key);
                    config->entries.emplace(key, HttpConfig::Entry{
                        Clock::now() + std::chrono::seconds(ttl), reply.decision,
                        reply.groups, config->order.begin()});
                }
            }
            groups = std::move(reply.groups);
            return reply.decision;
        }
        catch (const std::exception&)
        {
            // Never log transport exceptions: URLs, credentials or response bodies may appear
            // in parser and TLS diagnostics. Fail closed without leaking them.
            log(XMQ_LOG_WARNING, "HTTP authentication service unavailable or invalid response");
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }
    }

    bool stop() override
    {
        const std::scoped_lock lock(m_mutex);
        m_config.reset();
        return true;
    }

protected:
    virtual HttpReply fetch(const HttpConfig& config, const sptk::Buffer& payload)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(config.timeout);
        const auto remaining = [&]() {
            const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (value.count() <= 0) throw std::runtime_error("HTTP auth deadline exceeded");
            return value;
        };
        const sptk::URL url(config.endpoint);
        auto [host, port] = url.hostAndPort();
        if (!port) port = url.protocol() == "https" ? 443 : 80;
        std::shared_ptr<sptk::TCPSocket> socket;
        if (url.protocol() == "https")
        {
            auto tls = std::make_shared<sptk::SSLSocket>();
            tls->configureClientVerification(sptk::String(host), sptk::String(config.caFile));
            socket = std::move(tls);
        }
        else socket = std::make_shared<sptk::TCPSocket>();
        struct Close {
            std::shared_ptr<sptk::TCPSocket> socket;
            ~Close() { socket->close(); }
        } close{socket};
        socket->open(sptk::Host(host, port), sptk::Socket::OpenMode::CONNECT, false, remaining());
        socket->setIoDeadline(deadline); // cmd_post's internal waits obey the same socket deadline.
        sptk::HttpConnect http(socket);
        http.setResponseLimits(16 * 1024, 64 * 1024);
        http.requestHeaders()["Content-Type"] = "application/json";
        http.requestHeaders()["Accept"] = "application/json";
        http.requestHeaders()["Accept-Encoding"] = "identity";
        http.requestHeaders()["Connection"] = "close";
        if (!config.bearer.empty())
            http.requestHeaders()["Authorization"] = sptk::String("Bearer " + config.bearer);
        sptk::Buffer response;
        const auto status = http.cmd_post(sptk::String(url.requestTarget()), sptk::HttpParams(),
                                          payload, response, sptk::Strings(), nullptr, remaining());
        return parseReply(status, response);
    }

private:
    int number(const char* name, const char* fallback, int minimum, int maximum) const
    {
        const auto text = setting(name, fallback);
        int value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            value < minimum || value > maximum)
            throw std::runtime_error(std::string("Invalid HTTP auth setting: ") + name);
        return value;
    }
    std::mutex m_mutex;
    std::shared_ptr<HttpConfig> m_config;
    std::atomic<int> m_inflight{0};
};
} // namespace

const xmq_setting httpAuthSettings[]{
    {.name="url", .label="Authentication URL", .description="JSON POST endpoint; HTTPS outside loopback", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=1},
    {.name="ca_file", .label="CA certificate", .description="Empty uses system trust", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="bearer_token", .label="Service bearer token", .description="Optional Authorization credential sent only to this endpoint", .type=XMQ_SETTING_SECRET, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="username_prefix", .label="Username prefix", .description="Only matching names are handled; prefix is stripped in the HTTP request. Empty handles all named clients", .type=XMQ_SETTING_STRING, .default_value="http/", .choices=nullptr, .required=0},
    {.name="timeout_seconds", .label="Operation timeout", .description="Shared connection/TLS/HTTP budget; DNS cannot be interrupted", .type=XMQ_SETTING_INTEGER, .default_value="5", .choices=nullptr, .required=0},
    {.name="max_inflight", .label="Concurrent HTTP requests", .description="Excess uncached CONNECTs fail as subsystem errors", .type=XMQ_SETTING_INTEGER, .default_value="16", .choices=nullptr, .required=0},
    {.name="cache_ttl_seconds", .label="ALLOW cache lifetime", .description="0 disables; account changes apply after TTL and reconnect", .type=XMQ_SETTING_INTEGER, .default_value="60", .choices=nullptr, .required=0},
    {.name="negative_cache_ttl_seconds", .label="DENY cache lifetime", .description="0 disables; outages and not_handled are never cached", .type=XMQ_SETTING_INTEGER, .default_value="10", .choices=nullptr, .required=0},
    {.name="cache_max_entries", .label="Cache entries", .description="Bounded LRU decisions; no plaintext credentials stored in cache", .type=XMQ_SETTING_INTEGER, .default_value="4096", .choices=nullptr, .required=0},
};
XMQ_DEFINE_EXTENSION_DESCRIBED(HttpAuthenticator, "http-auth", "0.1", XMQ_CAP_AUTHENTICATOR,
                               "HTTP authentication with native XMQ group permissions", httpAuthSettings, "authentication")
