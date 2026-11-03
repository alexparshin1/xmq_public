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
 * @file Webhook.cpp
 * @brief Posts accepted MQTT messages to an HTTP(S) endpoint.
 *
 * The first data-integration connector: a message tap with a topic filter and a URL. The body is
 * JSON with the payload in base64, so a binary message survives the trip; the request is signed
 * with HMAC-SHA256 when a secret is configured; a failed request is retried with backoff.
 *
 * POSIX sockets and OpenSSL, because this build's platforms are Linux and BSD. A Windows port
 * needs the same structure over Winsock, which is a separate piece of work rather than a hidden
 * dependency here.
 */

#include "extension/XmqExtension.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int defaultTimeoutMs = 5000;
constexpr int defaultRetries = 3;

struct Url
{
    bool        m_tls {true};
    std::string m_host;
    std::string m_port;
    std::string m_path {"/"};
};

bool parseUrl(const std::string& text, Url& url)
{
    if (text.starts_with("https://"))
    {
        url.m_tls = true;
        url.m_path = text.substr(8);
    }
    else if (text.starts_with("http://"))
    {
        url.m_tls = false;
        url.m_path = text.substr(7);
    }
    else
    {
        return false;
    }
    if (url.m_path.empty())
    {
        return false;
    }

    const auto slash = url.m_path.find('/');
    const auto authority = url.m_path.substr(0, slash);
    url.m_path = slash == std::string::npos ? "/" : url.m_path.substr(slash);
    if (authority.empty())
    {
        return false;
    }
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos)
    {
        url.m_host = authority;
        url.m_port = url.m_tls ? "443" : "80";
    }
    else
    {
        url.m_host = authority.substr(0, colon);
        url.m_port = authority.substr(colon + 1);
        if (url.m_host.empty() || url.m_port.empty())
        {
            return false;
        }
    }
    return true;
}

std::string jsonEscape(const std::string_view text)
{
    std::string out;
    for (const char character: text)
    {
        switch (character)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                    out += buffer;
                }
                else
                {
                    out += character;
                }
        }
    }
    return out;
}

std::string base64(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                         reinterpret_cast<const unsigned char*>(bytes.data()),
                                         static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string hex(const unsigned char* bytes, const size_t count)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t at = 0; at < count; ++at)
    {
        out += digits[bytes[at] >> 4U];
        out += digits[bytes[at] & 0x0FU];
    }
    return out;
}

std::string isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm    utc {};
    gmtime_r(&time, &utc);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

int connectTcp(const Url& url, const int timeoutMs)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(url.m_host.c_str(), url.m_port.c_str(), &hints, &addresses) != 0)
    {
        return -1;
    }

    int socketHandle = -1;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next)
    {
        socketHandle = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketHandle < 0)
        {
            continue;
        }

        const int flags = fcntl(socketHandle, F_GETFL, 0);
        fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK);
        const int connected = ::connect(socketHandle, address->ai_addr, address->ai_addrlen);
        if (connected == 0)
        {
            fcntl(socketHandle, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS)
        {
            ::close(socketHandle);
            socketHandle = -1;
            continue;
        }

        pollfd descriptor {.fd = socketHandle, .events = POLLOUT, .revents = 0};
        if (poll(&descriptor, 1, timeoutMs) <= 0)
        {
            ::close(socketHandle);
            socketHandle = -1;
            continue;
        }
        int       error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
        {
            ::close(socketHandle);
            socketHandle = -1;
            continue;
        }
        fcntl(socketHandle, F_SETFL, flags);
        break;
    }
    freeaddrinfo(addresses);

    if (socketHandle >= 0)
    {
        timeval timeout {.tv_sec = timeoutMs / 1000, .tv_usec = (timeoutMs % 1000) * 1000};
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    return socketHandle;
}

class Connection
{
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { close(); }

    bool open(const Url& url, const int timeoutMs)
    {
        m_timeout = timeoutMs;
        m_socket = connectTcp(url, timeoutMs);
        if (m_socket < 0)
        {
            m_error = "cannot connect to " + url.m_host + ":" + url.m_port;
            return false;
        }
        if (!url.m_tls)
        {
            return true;
        }

        static std::once_flag once;
        std::call_once(once, []
        {
            SSL_library_init();
            SSL_load_error_strings();
        });

        m_context = SSL_CTX_new(TLS_client_method());
        if (m_context == nullptr)
        {
            m_error = "cannot create a TLS context";
            return false;
        }
        SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
        SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(m_context) <= 0)
        {
            m_error = "no system CA certificates are available";
            return false;
        }

        m_ssl = SSL_new(m_context);
        if (m_ssl == nullptr)
        {
            m_error = "cannot create a TLS session";
            return false;
        }
        SSL_set_fd(m_ssl, m_socket);
        SSL_set_tlsext_host_name(m_ssl, url.m_host.c_str());
        SSL_set1_host(m_ssl, url.m_host.c_str());
        if (SSL_connect(m_ssl) != 1)
        {
            m_error = "TLS handshake with " + url.m_host + " failed";
            return false;
        }
        return true;
    }

    bool writeAll(const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const int written = m_ssl != nullptr
                ? SSL_write(m_ssl, data.data() + sent, static_cast<int>(data.size() - sent))
                : static_cast<int>(::send(m_socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
            if (written <= 0)
            {
                m_error = "the endpoint stopped reading";
                return false;
            }
            sent += static_cast<size_t>(written);
        }
        return true;
    }

    std::string readAll()
    {
        std::string response;
        char        buffer[4096];
        while (true)
        {
            const int received = m_ssl != nullptr ? SSL_read(m_ssl, buffer, sizeof(buffer))
                                                  : static_cast<int>(::recv(m_socket, buffer, sizeof(buffer), 0));
            if (received <= 0)
            {
                break;
            }
            response.append(buffer, static_cast<size_t>(received));
            if (response.size() > 1'000'000)
            {
                break;
            }
        }
        return response;
    }

    void close()
    {
        if (m_ssl != nullptr)
        {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        if (m_context != nullptr)
        {
            SSL_CTX_free(m_context);
            m_context = nullptr;
        }
        if (m_socket >= 0)
        {
            ::close(m_socket);
            m_socket = -1;
        }
    }

    [[nodiscard]] const std::string& error() const { return m_error; }

private:
    int         m_socket {-1};
    int         m_timeout {defaultTimeoutMs};
    SSL_CTX*    m_context {nullptr};
    SSL*        m_ssl {nullptr};
    std::string m_error;
};

int post(const Url& url, const std::string& body, const std::string& secret, const int timeoutMs,
         std::string& error)
{
    Connection connection;
    if (!connection.open(url, timeoutMs))
    {
        error = connection.error();
        return -1;
    }

    std::string request = "POST " + url.m_path + " HTTP/1.1\r\n" +
        "Host: " + url.m_host + "\r\n" +
        "Content-Type: application/json\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "User-Agent: xmq-webhook/1.0\r\n" +
        "Connection: close\r\n";
    if (!secret.empty())
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  length = 0;
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &length);
        request += "X-XMQ-Signature: sha256=" + hex(digest, length) + "\r\n";
    }
    request += "\r\n";
    request += body;

    if (!connection.writeAll(request))
    {
        error = connection.error();
        return -1;
    }
    const auto response = connection.readAll();
    if (response.empty())
    {
        error = "the endpoint sent no response";
        return -1;
    }
    const auto firstLine = response.find("\r\n");
    const auto status = firstLine == std::string::npos ? response : response.substr(0, firstLine);
    if (status.size() < 12 || !status.starts_with("HTTP/"))
    {
        error = "the endpoint sent a malformed response";
        return -1;
    }
    try
    {
        return std::stoi(status.substr(9, 3));
    }
    catch (const std::exception&)
    {
        error = "the endpoint sent a malformed status";
        return -1;
    }
}

class Webhook : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        return configure();
    }

    bool reload() override
    {
        return configure();
    }

    void onMessage(const xmq_message& message) override
    {
        Config config;
        {
            const std::shared_lock lock(m_configLock);
            config = m_config;
        }

        const std::string payload(message.payload.data, message.payload.length);
        const std::string body =
            "{\"id\":\"" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch()).count()) +
            "-" + std::to_string(m_sequence.fetch_add(1)) + "\"" +
            ",\"version\":1" +
            ",\"timestamp\":\"" + isoTimestamp() + "\"" +
            ",\"topic\":\"" + jsonEscape(xmq::view(message.topic)) + "\"" +
            ",\"qos\":" + std::to_string(static_cast<unsigned>(message.qos)) +
            ",\"retain\":" + (message.retain != 0 ? "true" : "false") +
            ",\"client_id\":\"" + jsonEscape(xmq::view(message.client_id)) + "\"" +
            ",\"username\":\"" + jsonEscape(xmq::view(message.username)) + "\"" +
            ",\"payload_base64\":\"" + base64(payload) + "\"}";

        for (int attempt = 0; attempt <= config.m_retries; ++attempt)
        {
            std::string error;
            const int status = post(config.m_url, body, config.m_secret, config.m_timeoutMs, error);
            if (status >= 200 && status < 300)
            {
                return;
            }
            if (status >= 400 && status < 500 && status != 429)
            {
                log(XMQ_LOG_WARNING, "the endpoint refused the message: HTTP " + std::to_string(status));
                return;
            }
            if (!error.empty())
            {
                log(XMQ_LOG_WARNING, "delivery failed: " + error);
            }
            if (attempt < config.m_retries)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(2000, 100 << attempt)));
            }
        }
        log(XMQ_LOG_ERROR, std::string("a message for ") + std::string(xmq::view(message.topic)) +
                               " was not delivered");
    }

private:
    struct Config
    {
        Url         m_url;
        std::string m_secret;
        int         m_timeoutMs {defaultTimeoutMs};
        int         m_retries {defaultRetries};
    };

    bool configure()
    {
        Config config;
        const auto url = setting("url");
        if (!parseUrl(url, config.m_url))
        {
            log(XMQ_LOG_ERROR, "url must be an http:// or https:// address");
            return false;
        }
        if (!config.m_url.m_tls && setting("allow_http", "false") != "true")
        {
            log(XMQ_LOG_ERROR, "plain HTTP is refused unless allow_http is true");
            return false;
        }
        config.m_secret = setting("secret");
        try
        {
            config.m_timeoutMs = std::stoi(setting("timeout_ms", "5000"));
            config.m_retries = std::stoi(setting("retries", "3"));
        }
        catch (const std::exception&)
        {
            log(XMQ_LOG_ERROR, "timeout_ms and retries must be numbers");
            return false;
        }
        if (config.m_timeoutMs <= 0 || config.m_retries < 0)
        {
            log(XMQ_LOG_ERROR, "timeout_ms must be positive and retries cannot be negative");
            return false;
        }

        {
            const std::unique_lock lock(m_configLock);
            m_config = std::move(config);
        }
        return subscribeMessages(setting("topic_filter", "#"),
                                 static_cast<size_t>(std::stoull(setting("max_payload", "1048576"))));
    }

    mutable std::shared_mutex m_configLock;
    Config                    m_config;
    std::atomic<uint64_t>     m_sequence {0};
};

constexpr xmq_setting settings[] = {
    {"url", "URL", "Where accepted messages are posted, http:// or https://", XMQ_SETTING_STRING, "",
     nullptr, 1},
    {"secret", "Secret", "HMAC-SHA256 key for the X-XMQ-Signature header", XMQ_SETTING_SECRET, "",
     nullptr, 0},
    {"topic_filter", "Topic filter", "Only messages matching this MQTT filter are sent", XMQ_SETTING_STRING,
     "#", nullptr, 0},
    {"max_payload", "Max payload", "Largest body to send, in bytes", XMQ_SETTING_INTEGER, "1048576",
     nullptr, 0},
    {"timeout_ms", "Timeout", "HTTP connect and read timeout, in milliseconds", XMQ_SETTING_INTEGER,
     "5000", nullptr, 0},
    {"retries", "Retries", "Attempts after the first one", XMQ_SETTING_INTEGER, "3", nullptr, 0},
    {"allow_http", "Allow HTTP", "Permit a plain HTTP URL (for a local receiver)", XMQ_SETTING_BOOLEAN,
     "false", nullptr, 0},
};

} // namespace

XMQ_DEFINE_EXTENSION_DESCRIBED(Webhook, "webhook", "1.0", XMQ_CAP_MESSAGE_TAP,
                               "Posts accepted MQTT messages to an HTTP(S) endpoint.", settings)
