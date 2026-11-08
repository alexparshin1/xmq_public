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
 * Sockets through the platform's own API and OpenSSL for TLS: Winsock on Windows, POSIX sockets
 * everywhere else. The rest of the file - the queue, the retries, the templates - is the same on
 * both.
 */

#include "extension/XmqExtension.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int defaultTimeoutMs = 5000;
constexpr int defaultRetries = 3;

/// Winsock and POSIX sockets differ in the handle's type, in how it is closed, in how it is made
/// non-blocking and in the name of the error that means "the connect is still running". Everything
/// else in this file goes through one of these and a single connect/read/write path.
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalidSocket = -1;
#endif

bool startSockets()
{
#ifdef _WIN32
    // Once per process: the host loads several extensions, and a second WSAStartup without a
    // matching WSACleanup is not an error, but there is no reason to make it one.
    static const bool ready = []
    {
        WSADATA data {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
#else
    return true;
#endif
}

void closeSocket(const socket_t handle)
{
    if (handle == invalidSocket)
    {
        return;
    }
#ifdef _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

bool setNonBlocking(const socket_t handle, const bool nonBlocking)
{
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ::ioctlsocket(handle, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return ::fcntl(handle, F_SETFL,
                   nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

int lastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool connectInProgress(const int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS;
#endif
}

int compareCaseInsensitive(const char* left, const char* right, const size_t count)
{
#ifdef _WIN32
    return ::_strnicmp(left, right, count);
#else
    return ::strncasecmp(left, right, count);
#endif
}

/// A send to a socket the peer has closed must fail rather than raise SIGPIPE. POSIX has a flag
/// for it; Winsock never raises the signal, so zero is what it wants.
#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif

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

    if (const auto colon = authority.rfind(':');
        colon == std::string::npos)
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
            case '"':
                out += R"(\")";
                break;
            case '\\':
                out += R"(\\)";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
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
    const auto  written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                          reinterpret_cast<const unsigned char*>(bytes.data()),
                                          static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string hex(const unsigned char* bytes, const size_t count)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string           out;
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
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

socket_t connectTcp(const Url& url, const int timeoutMs)
{
    if (!startSockets())
    {
        return invalidSocket;
    }

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(url.m_host.c_str(), url.m_port.c_str(), &hints, &addresses) != 0)
    {
        return invalidSocket;
    }

    socket_t socketHandle = invalidSocket;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next)
    {
        socketHandle = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketHandle == invalidSocket)
        {
            continue;
        }

        if (!setNonBlocking(socketHandle, true))
        {
            closeSocket(socketHandle);
            socketHandle = invalidSocket;
            continue;
        }
        const int connected = ::connect(socketHandle, address->ai_addr,
                                        static_cast<int>(address->ai_addrlen));
        if (connected == 0)
        {
            setNonBlocking(socketHandle, false);
            break;
        }
        if (!connectInProgress(lastSocketError()))
        {
            closeSocket(socketHandle);
            socketHandle = invalidSocket;
            continue;
        }

        // select() is the portable way to wait for a non-blocking connect: WSAPoll has been wrong
        // on Windows for exactly this case, and POSIX has select() everywhere.
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socketHandle, &writeSet);
        timeval timeout {};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
        const int descriptorCount = 0; // ignored by Winsock
#else
        const int descriptorCount = socketHandle + 1;
#endif
        const int selected = ::select(descriptorCount, nullptr, &writeSet, nullptr, &timeout);
        if (selected <= 0 || !FD_ISSET(socketHandle, &writeSet))
        {
            closeSocket(socketHandle);
            socketHandle = invalidSocket;
            continue;
        }
        int error = 0;
#ifdef _WIN32
        int length = sizeof(error);
#else

#endif
        if (socklen_t length = sizeof(error);
            getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &length) != 0 ||
            error != 0)
        {
            closeSocket(socketHandle);
            socketHandle = invalidSocket;
            continue;
        }
        setNonBlocking(socketHandle, false);
        break;
    }
    freeaddrinfo(addresses);

    if (socketHandle != invalidSocket)
    {
        // Blocking reads and writes still need a deadline: a receiver that stops reading must not
        // hold a worker forever. Winsock takes milliseconds here, POSIX a timeval.
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeoutMs);
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout {};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }
    return socketHandle;
}

/// Header names are case-insensitive, and an endpoint may answer in any case.
size_t lowerCaseFind(const std::string& text, const char* needle)
{
    const size_t length = strlen(needle);
    for (size_t at = 0; at + length <= text.size(); ++at)
    {
        if (compareCaseInsensitive(text.c_str() + at, needle, length) == 0)
        {
            return at;
        }
    }
    return std::string::npos;
}

class Connection
{
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection()
    { close(); }

    bool open(const Url& url, const int timeoutMs)
    {
        m_timeout = timeoutMs;
        m_socket = connectTcp(url, timeoutMs);
        if (m_socket == invalidSocket)
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
        SSL_set_fd(m_ssl, static_cast<int>(m_socket));
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
                                    : static_cast<int>(::send(m_socket, data.data() + sent,
                                                              static_cast<int>(data.size() - sent), sendFlags));
            if (written <= 0)
            {
                m_error = "the endpoint stopped reading";
                return false;
            }
            sent += static_cast<size_t>(written);
        }
        return true;
    }

    /**
     * @brief One response, headers and the body its Content-Length promised.
     *
     * Keeps the connection for the next request unless the endpoint said it would close, which is
     * what makes a burst of small records affordable: a message per TCP connection runs into
     * ephemeral ports long before it runs into CPU.
     */
    bool readResponse(int& status, std::string& error)
    {
        std::string response;
        char        buffer[8192];

        size_t headerEnd = std::string::npos;
        size_t bodyLength = 0;
        while (true)
        {
            if (headerEnd == std::string::npos)
            {
                headerEnd = response.find("\r\n\r\n");
                if (headerEnd != std::string::npos)
                {
                    const std::string head = response.substr(0, headerEnd);
                    if (head.size() < 12 || !head.starts_with("HTTP/"))
                    {
                        error = "the endpoint sent a malformed response";
                        return false;
                    }
                    try
                    {
                        status = std::stoi(head.substr(9, 3));
                    }
                    catch (const std::exception&)
                    {
                        error = "the endpoint sent a malformed status";
                        return false;
                    }

                    if (const auto lengthAt = lowerCaseFind(head, "content-length:");
                        lengthAt != std::string::npos)
                    {
                        bodyLength = std::strtoul(head.c_str() + lengthAt + 15, nullptr, 10);
                    }

                    const auto connection = lowerCaseFind(head, "connection:");
                    if (connection != std::string::npos &&
                        lowerCaseFind(head.substr(connection), "close") != std::string::npos)
                    {
                        m_closeAfterResponse = true;
                    }
                }
            }

            if (headerEnd != std::string::npos &&
                response.size() >= headerEnd + 4 + bodyLength)
            {
                return true;
            }

            const int received = m_ssl != nullptr ? SSL_read(m_ssl, buffer, sizeof(buffer))
                                                  : static_cast<int>(::recv(m_socket, buffer, sizeof(buffer), 0));
            if (received <= 0)
            {
                error = response.empty() ? "the endpoint sent no response"
                                         : "the endpoint closed the connection mid-response";
                m_closeAfterResponse = true;
                return false;
            }
            response.append(buffer, static_cast<size_t>(received));
            if (response.size() > 4 * 1024 * 1024)
            {
                error = "the endpoint sent an oversized response";
                m_closeAfterResponse = true;
                return false;
            }
        }
    }

    [[nodiscard]] bool closeAfterResponse() const
    { return m_closeAfterResponse; }
    void close()
    {
        m_closeAfterResponse = false;
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
        if (m_socket != invalidSocket)
        {
            closeSocket(m_socket);
            m_socket = invalidSocket;
        }
    }

    [[nodiscard]] const std::string& error() const
    { return m_error; }
    [[nodiscard]] bool isOpen() const
    { return m_socket != invalidSocket; }

private:
    socket_t    m_socket {invalidSocket};
    int         m_timeout {defaultTimeoutMs};
    SSL_CTX*    m_context {nullptr};
    SSL*        m_ssl {nullptr};
    bool        m_closeAfterResponse {false};
    std::string m_error;
};

/**
 * @brief One destination's side of the conversation.
 *
 * The connection is kept open between records. Opening one per record was measured at roughly
 * twenty thousand a second on loopback, where the limit is ephemeral ports and TIME_WAIT, not CPU -
 * a connector posting a log storm hits that ceiling long before the broker does.
 *
 * A connection the endpoint closed while idle is not an error the caller should see: the first
 * attempt fails, the connection is re-established, and the record is sent once more.
 */
class Peer
{
public:
    Peer(Url url, std::string secret, const int timeoutMs,
         std::vector<std::pair<std::string, std::string>> headers)
        : m_url(std::move(url))
        , m_secret(std::move(secret))
        , m_timeoutMs(timeoutMs)
        , m_headers(std::move(headers))
    {
    }

    int send(const std::string& body, std::string& error)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!m_connection.isOpen() && !m_connection.open(m_url, m_timeoutMs))
            {
                error = m_connection.error();
                return -1;
            }

            std::string request = format("POST {} HTTP/1.1\r\n"
                                         "Host: {}\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: {}\r\n"
                                         "User-Agent: xmq-webhook/1.2\r\n"
                                         "Connection: keep-alive\r\n",
                                         m_url.m_path, m_url.m_host, body.size());
            for (const auto& [name, value]: m_headers)
            {
                request += name + ": " + value + "\r\n";
            }
            if (!m_secret.empty())
            {
                unsigned char digest[EVP_MAX_MD_SIZE];
                unsigned int  length = 0;
                HMAC(EVP_sha256(), m_secret.data(), static_cast<int>(m_secret.size()),
                     reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &length);
                request += "X-XMQ-Signature: sha256=" + hex(digest, length) + "\r\n";
            }
            request += "\r\n";
            request += body;

            if (!m_connection.writeAll(request))
            {
                error = m_connection.error();
                m_connection.close();
                continue; // a connection that died while idle looks exactly like this
            }

            int status = -1;
            if (!m_connection.readResponse(status, error))
            {
                m_connection.close();
                continue;
            }
            if (m_connection.closeAfterResponse())
            {
                m_connection.close();
            }
            return status;
        }
        return -1;
    }

private:
    Url                                              m_url;
    std::string                                      m_secret;
    int                                              m_timeoutMs {defaultTimeoutMs};
    std::vector<std::pair<std::string, std::string>> m_headers;
    Connection                                       m_connection;
};


namespace {

/// A JSON value as far as this connector needs one: the list settings are arrays of strings. A
/// full parser would be a dependency; this is the smallest thing that can read them without
/// guessing.
struct JsonValue
{
    enum class Kind
    {
        Null,
        String,
        Number,
        Boolean,
        Object,
        Array
    };

    Kind                                           kind {Kind::Null};
    std::string                                    text;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue>                         array;
};

class JsonReader
{
public:
    explicit JsonReader(const std::string_view text)
        : m_text(text)
    {
    }

    bool read(JsonValue& value)
    {
        skipSpace();
        return readValue(value) && (skipSpace(), true);
    }

    [[nodiscard]] const std::string& error() const
    { return m_error; }

private:
    void skipSpace()
    {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
        {
            ++m_at;
        }
    }

    bool fail(const std::string& what)
    {
        m_error = what + " at offset " + std::to_string(m_at);
        return false;
    }

    bool readValue(JsonValue& value)
    {
        if (m_at >= m_text.size())
        {
            return fail("the value ended early");
        }
        switch (m_text[m_at])
        {
            case '{':
                return readObject(value);
            case '[':
                return readArray(value);
            case '"': {
                value.kind = JsonValue::Kind::String;
                return readString(value.text);
            }
            case 't':
            case 'f': {
                const auto word = m_text.substr(m_at, 4);
                const bool isTrue = word.starts_with("true");
                if (!isTrue && !word.starts_with("false"))
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Boolean;
                value.text = isTrue ? "true" : "false";
                m_at += isTrue ? 4 : 5;
                return true;
            }
            case 'n': {
                if (!m_text.substr(m_at).starts_with("null"))
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Null;
                m_at += 4;
                return true;
            }
            default: {
                const auto start = m_at;
                while (m_at < m_text.size() &&
                       (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || m_text[m_at] == '-' ||
                        m_text[m_at] == '+' || m_text[m_at] == '.' || m_text[m_at] == 'e' ||
                        m_text[m_at] == 'E'))
                {
                    ++m_at;
                }
                if (start == m_at)
                {
                    return fail("not a JSON value");
                }
                value.kind = JsonValue::Kind::Number;
                value.text = std::string(m_text.substr(start, m_at - start));
                return true;
            }
        }
    }

    bool readObject(JsonValue& value)
    {
        value.kind = JsonValue::Kind::Object;
        ++m_at; // '{'
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '}')
        {
            ++m_at;
            return true;
        }
        while (true)
        {
            skipSpace();
            std::string name;
            if (!readString(name))
            {
                return false;
            }
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ':')
            {
                return fail("expected ':'");
            }
            ++m_at;
            skipSpace();
            JsonValue member;
            if (!readValue(member))
            {
                return false;
            }
            value.object.emplace_back(std::move(name), std::move(member));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',')
            {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == '}')
            {
                ++m_at;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool readArray(JsonValue& value)
    {
        value.kind = JsonValue::Kind::Array;
        ++m_at; // '['
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == ']')
        {
            ++m_at;
            return true;
        }
        while (true)
        {
            skipSpace();
            JsonValue element;
            if (!readValue(element))
            {
                return false;
            }
            value.array.push_back(std::move(element));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',')
            {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == ']')
            {
                ++m_at;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool readString(std::string& out)
    {
        if (m_at >= m_text.size() || m_text[m_at] != '"')
        {
            return fail("expected a string");
        }
        ++m_at;
        out.clear();
        while (m_at < m_text.size())
        {
            const char character = m_text[m_at++];
            if (character == '"')
            {
                return true;
            }
            if (character != '\\')
            {
                out += character;
                continue;
            }
            if (m_at >= m_text.size())
            {
                return fail("a string ended in the middle of an escape");
            }
            switch (m_text[m_at++])
            {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned code = 0;
                    for (int digit = 0; digit < 4; ++digit)
                    {
                        code <<= 4U;
                        const char hexDigit = m_at < m_text.size() ? m_text[m_at++] : '0';
                        code |= hexDigit >= '0' && hexDigit <= '9'   ? static_cast<unsigned>(hexDigit - '0')
                                : hexDigit >= 'a' && hexDigit <= 'f' ? static_cast<unsigned>(hexDigit - 'a' + 10)
                                : hexDigit >= 'A' && hexDigit <= 'F' ? static_cast<unsigned>(hexDigit - 'A' + 10)
                                                                     : 0U;
                    }
                    // UTF-8, for the ranges a settings file is likely to hold; a surrogate pair is
                    // left as two replacements rather than guessed at.
                    if (code < 0x80)
                    {
                        out += static_cast<char>(code);
                    }
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (code >> 6U));
                        out += static_cast<char>(0x80 | (code & 0x3FU));
                    }
                    else
                    {
                        out += static_cast<char>(0xE0 | (code >> 12U));
                        out += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
                        out += static_cast<char>(0x80 | (code & 0x3FU));
                    }
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
        return fail("a string was not closed");
    }

    const std::string_view m_text;
    size_t                 m_at {0};
    std::string            m_error;
};

/// MQTT topic matching, because the host is given one filter for the whole extension while each
/// destination has its own: the host filter is "#" and the narrowing happens here.
bool topicMatches(const std::string_view filter, const std::string_view topic)
{
    size_t at = 0;
    size_t topicAt = 0;
    while (true)
    {
        const auto filterEnd = filter.find('/', at);
        const auto topicEnd = topic.find('/', topicAt);
        const auto filterLevel = filter.substr(at, filterEnd == std::string_view::npos
                                                       ? std::string_view::npos
                                                       : filterEnd - at);
        const auto topicLevel = topic.substr(topicAt, topicEnd == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : topicEnd - topicAt);

        if (filterLevel == "#")
        {
            return true;
        }
        if (filterLevel != "+" && filterLevel != topicLevel)
        {
            return false;
        }

        const bool filterLast = filterEnd == std::string_view::npos;
        const bool topicLast = topicEnd == std::string_view::npos;
        if (filterLast && topicLast)
        {
            return true;
        }

        if (filterLast)
        {
            return false;
        }

        if (topicLast)
        {
            // Mosquitto's "sport/#" covers "sport" itself.
            return filter.ends_with("#");
        }

        at = filterEnd + 1;
        topicAt = topicEnd + 1;
    }
}

/// Whitespace around a setting is an artefact of how the file was written, not part of its value.
std::string trimmed(const std::string_view text)
{
    size_t first = 0;
    size_t last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
    {
        ++first;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

/// One filter, or a JSON array of them.
///
/// The common case stays a plain string, so the file reads as it did. The array form exists
/// because a comma cannot separate filters: it is a legal character in a topic, and "a,b" would be
/// either one topic or two filters with nothing to say which. A value that starts with '[' says
/// what it is.
bool parseTopicFilters(const std::string_view text, std::vector<std::string>& filters, std::string& error)
{
    filters.clear();
    const auto value = trimmed(text);
    if (value.empty())
    {
        filters.emplace_back("#");
        return true;
    }
    if (value.front() != '[')
    {
        filters.push_back(value);
        return true;
    }

    JsonValue  parsed;
    JsonReader reader(value);
    if (!reader.read(parsed) || parsed.kind != JsonValue::Kind::Array)
    {
        error = "topic_filter must be an MQTT filter or a JSON array of them";
        return false;
    }
    for (const auto& element: parsed.array)
    {
        if (element.kind != JsonValue::Kind::String || element.text.empty())
        {
            error = "topic_filter must hold only non-empty strings";
            return false;
        }
        filters.push_back(element.text);
    }
    if (filters.empty())
    {
        error = "topic_filter must name at least one filter";
        return false;
    }
    return true;
}

/// Extra request headers, one "Name: value" per item; a single string is accepted too.
///
/// The same shape as topic_filter: the interface edits a list, the file stores a JSON array, and a
/// value written by hand as one header still works.
bool parseHeaders(const std::string_view text, std::vector<std::pair<std::string, std::string>>& headers,
                  std::string& error)
{
    headers.clear();
    const auto value = trimmed(text);
    if (value.empty())
    {
        return true;
    }

    std::vector<std::string> items;
    if (value.front() == '[')
    {
        JsonValue parsed;

        if (JsonReader reader(value);
            !reader.read(parsed) || parsed.kind != JsonValue::Kind::Array)
        {
            error = "headers must be a header or a JSON array of them";
            return false;
        }
        for (const auto& element: parsed.array)
        {
            if (element.kind != JsonValue::Kind::String)
            {
                error = "headers must hold only strings";
                return false;
            }
            items.push_back(element.text);
        }
    }
    else
    {
        items.push_back(value);
    }

    const auto isHeaderName = [](const std::string& name)
    {
        return !name.empty() && std::ranges::all_of(name,
                                                    [](const unsigned char ch)
                                                    {
                                                        return std::isalnum(ch) != 0 || ch == '!' || ch == '#' || ch == '$' ||
                                                               ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
                                                               ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' ||
                                                               ch == '|' || ch == '~';
                                                    });
    };

    for (const auto& item: items)
    {
        const auto colon = item.find(':');
        if (colon == std::string::npos)
        {
            error = "header '" + item + "' must be Name: value";
            return false;
        }
        const auto name = trimmed(std::string_view(item).substr(0, colon));
        const auto field = trimmed(std::string_view(item).substr(colon + 1));
        if (!isHeaderName(name))
        {
            error = "header name '" + name + "' is not an HTTP field name";
            return false;
        }
        if (field.find_first_of("\r\n") != std::string::npos)
        {
            error = "header '" + name + "' holds a line break";
            return false;
        }
        headers.emplace_back(name, field);
    }
    return true;
}

/// Replaces {{name}} with the JSON-escaped value, so a template stays valid JSON whatever the text
/// holds. An unknown name is left alone rather than silently emptied.
std::string renderTemplate(const std::string&                                      tmpl,
                           const std::vector<std::pair<std::string, std::string>>& values)
{
    std::string out;
    size_t      at = 0;
    while (at < tmpl.size())
    {
        const auto open = tmpl.find("{{", at);
        if (open == std::string::npos)
        {
            out += tmpl.substr(at);
            break;
        }
        out += tmpl.substr(at, open - at);
        const auto close = tmpl.find("}}", open + 2);
        if (close == std::string::npos)
        {
            out += tmpl.substr(open);
            break;
        }
        const auto name = tmpl.substr(open + 2, close - open - 2);
        const auto found = std::ranges::find_if(values,
                                                [&name](const auto& entry)
                                                {
                                                    return entry.first == name;
                                                });
        if (found == values.end())
        {
            out += tmpl.substr(open, close + 2 - open);
        }
        else
        {
            out += jsonEscape(found->second);
        }
        at = close + 2;
    }
    return out;
}

/// What a body is when no template is set: one line per source, with the names people ask for
/// first. A message has no level of its own, so it says "message" - which is also what tells the
/// two sources apart at the receiver.
constexpr const char* defaultTemplate =
    R"({"topic":"{{topic}}","message":"{{message}}","level":"{{level}}"})";

/// The place to post to: URL, body template, filters, and its own queue and threads so a slow
/// receiver costs deliveries and not broker throughput.
struct Destination
{
    std::string                                      name;
    Url                                              url;
    std::string                                      secret;
    int                                              timeoutMs {defaultTimeoutMs};
    int                                              retries {defaultRetries};
    std::string                                      tmpl; ///< Never empty; the setting falls back to defaultTemplate.
    bool                                             sendMessages {true};
    bool                                             sendLog {false};
    std::vector<std::string>                         topicFilters {"#"};
    size_t                                           maxPayload {std::numeric_limits<size_t>::max()};
    uint32_t                                         minLevel {XMQ_LOG_ERROR};
    std::vector<std::string>                         logSubjects;
    std::vector<std::pair<std::string, std::string>> headers;

    /// How many requests may be in flight at once. One connection per destination is enough on
    /// loopback and far too few across a network: a synchronous reply per record caps a destination
    /// at 1/RTT, which is a hundred records a second at 10 ms.
    int concurrency {4};

    std::mutex                                      lock;
    std::condition_variable                         condition;
    std::deque<std::pair<std::string, std::string>> queue; ///< body, what it was
    bool                                            stopping {false};
    std::vector<std::thread>                        threads;
    std::atomic<uint64_t>                           sent {0};
    std::atomic<uint64_t>                           failed {0};
    std::atomic<uint64_t>                           dropped {0};
};

constexpr size_t maxQueuedPerDestination = 1024;

/// 2xx is success; 4xx except 429 is refused without a second attempt; everything else retries
/// with backoff. The same policy for both inputs, in one place.
bool deliverOnce(Peer& peer, const Destination& destination, const std::string& body,
                 const std::string& what)
{
    for (int attempt = 0; attempt <= destination.retries; ++attempt)
    {
        std::string error;
        const int   status = peer.send(body, error);
        if (status >= 200 && status < 300)
        {
            return true;
        }
        if (status >= 400 && status < 500 && status != 429)
        {
            // The endpoint will say no again; a retry would only be noise.
            std::fprintf(stderr, "webhook: %s refused the message: HTTP %d\n", what.c_str(), status);
            return false;
        }
        if (!error.empty())
        {
            std::fprintf(stderr, "webhook: delivery of %s failed: %s\n", what.c_str(), error.c_str());
        }
        if (attempt < destination.retries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(2000, 100 << attempt)));
        }
    }
    return false;
}

} // namespace

class Webhook : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    ~Webhook() override
    {
        stopDestinations();
    }

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
        const auto destinations = snapshot();
        if (destinations.empty())
        {
            return;
        }

        const std::string                                topic(xmq::view(message.topic));
        const std::string                                id = nextId();
        const std::string                                timestamp = isoTimestamp();
        std::vector<std::pair<std::string, std::string>> values;

        for (const auto& destination: destinations)
        {
            if (!destination->sendMessages ||
                std::ranges::none_of(destination->topicFilters, [&topic](const std::string& filter)
                                     {
                                         return topicMatches(filter, topic);
                                     }) ||
                message.payload.length > destination->maxPayload)
            {
                continue;
            }
            if (values.empty())
            {
                const auto payload = base64(std::string(message.payload.data, message.payload.length));
                values = {
                    {"id", id},
                    {"source", "message"},
                    {"timestamp", timestamp},
                    {"topic", topic},
                    {"qos", std::to_string(static_cast<unsigned>(message.qos))},
                    {"retain", message.retain != 0 ? "true" : "false"},
                    {"client_id", std::string(xmq::view(message.client_id))},
                    {"username", std::string(xmq::view(message.username))},
                    {"payload_base64", payload},
                    // For a message the body is the base64 payload, and there is no level: the
                    // literal says which source this is.
                    {"message", payload},
                    {"level", "message"}};
            }
            enqueue(*destination, renderTemplate(destination->tmpl, values), "message for " + topic);
        }
    }

    /// One line of the broker's own log: the other input, so one endpoint can be told both what
    /// clients sent and what the broker could not do.
    void onLog(const xmq_log_record& record) override
    {
        const auto destinations = snapshot();
        if (destinations.empty())
        {
            return;
        }

        const std::string                                subject(xmq::view(record.subject));
        const std::string                                message(xmq::view(record.message));
        const std::string                                id = nextId();
        const std::string                                timestamp = isoTimestamp();
        std::vector<std::pair<std::string, std::string>> values;

        for (const auto& destination: destinations)
        {
            if (!destination->sendLog || record.level < destination->minLevel)
            {
                continue;
            }
            if (!destination->logSubjects.empty() &&
                std::ranges::none_of(destination->logSubjects,
                                     [&subject](const std::string& candidate)
                                     {
                                         return candidate == subject;
                                     }))
            {
                continue;
            }
            if (values.empty())
            {
                values = {
                    {"id", id},
                    {"source", "log"},
                    {"timestamp", timestamp},
                    {"topic", ""}, // a record has no topic; the default body keeps the field
                    {"level", levelName(record.level)},
                    {"subject", subject},
                    {"message", message}};
            }
            enqueue(*destination, renderTemplate(destination->tmpl, values),
                    "log record from " + subject);
        }
    }

private:
    static std::string levelName(const uint32_t level)
    {
        switch (level)
        {
            case XMQ_LOG_DEBUG:
                return "debug";
            case XMQ_LOG_INFO:
                return "info";
            case XMQ_LOG_WARNING:
                return "warning";
            default:
                return "error";
        }
    }

    static bool parseLevel(const std::string& text, uint32_t& level)
    {
        if (text == "debug")
        {
            level = XMQ_LOG_DEBUG;
            return true;
        }
        if (text == "info")
        {
            level = XMQ_LOG_INFO;
            return true;
        }
        if (text == "warning")
        {
            level = XMQ_LOG_WARNING;
            return true;
        }
        if (text == "error")
        {
            level = XMQ_LOG_ERROR;
            return true;
        }
        return false;
    }

    static std::vector<std::string> splitList(const std::string& text, const char separator)
    {
        std::vector<std::string> pieces;
        size_t                   at = 0;
        while (at <= text.size())
        {
            const auto end = text.find(separator, at);
            auto       piece = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            const auto first = piece.find_first_not_of(" \t");
            const auto last = piece.find_last_not_of(" \t");
            if (first != std::string::npos)
            {
                pieces.push_back(piece.substr(first, last - first + 1));
            }
            if (end == std::string::npos)
            {
                break;
            }
            at = end + 1;
        }
        return pieces;
    }

    std::string nextId()
    {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count()) +
               "-" + std::to_string(m_sequence.fetch_add(1));
    }

    std::vector<std::shared_ptr<Destination>> snapshot() const
    {
        const std::scoped_lock lock(m_destinationsLock);
        return m_current;
    }

    void enqueue(Destination& destination, std::string body, std::string what)
    {
        {
            const std::scoped_lock lock(destination.lock);
            if (destination.stopping || destination.queue.size() >= maxQueuedPerDestination)
            {
                destination.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            destination.queue.emplace_back(std::move(body), std::move(what));
        }
        destination.condition.notify_one();
    }

    static void run(std::shared_ptr<Destination> destination)
    {
        Peer peer(destination->url, destination->secret, destination->timeoutMs, destination->headers);
        while (true)
        {
            std::pair<std::string, std::string> entry;
            {
                std::unique_lock lock(destination->lock);
                destination->condition.wait(lock, [&destination]
                                            {
                                                return destination->stopping || !destination->queue.empty();
                                            });
                if (destination->queue.empty() && destination->stopping)
                {
                    return;
                }
                entry = std::move(destination->queue.front());
                destination->queue.pop_front();
            }
            if (deliverOnce(peer, *destination, entry.first, entry.second))
            {
                destination->sent.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                destination->failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void stopDestinations()
    {
        std::vector<std::shared_ptr<Destination>> retiring;
        {
            const std::scoped_lock lock(m_destinationsLock);
            retiring = std::move(m_current);
            m_current.clear();
        }
        for (const auto& destination: retiring)
        {
            {
                const std::scoped_lock lock(destination->lock);
                destination->stopping = true;
            }
            destination->condition.notify_all();
            for (auto& thread: destination->threads)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
            destination->threads.clear();
        }
    }

    /// A field that was cleared in the interface is not a value: the declared default applies.
    /// Apply writes every field it shows, so an unset number would otherwise reach stoi as "".
    std::string settingOr(const char* name, const char* fallback) const
    {
        const auto value = setting(name, fallback);
        return value.empty() ? std::string(fallback) : value;
    }

    /// The settings build the one endpoint this connector posts to.
    std::shared_ptr<Destination> configuredDestination(std::string& error) const
    {
        auto destination = std::make_shared<Destination>();
        destination->name = "destination 1";
        if (!parseUrl(setting("url"), destination->url))
        {
            error = "url must be an http:// or https:// address";
            return nullptr;
        }
        if (!destination->url.m_tls && setting("allow_http", "false") != "true")
        {
            error = "plain HTTP is refused unless allow_http is true";
            return nullptr;
        }
        destination->secret = setting("secret");
        destination->tmpl = settingOr("template", defaultTemplate);
        if (!parseHeaders(setting("headers"), destination->headers, error))
        {
            return nullptr;
        }
        try
        {
            destination->timeoutMs = std::stoi(settingOr("timeout_ms", "5000"));
            destination->retries = std::stoi(settingOr("retries", "3"));
            const auto maxPayload = settingOr("max_payload", "1048576");
            destination->maxPayload = std::stoull(maxPayload) == 0
                                          ? std::numeric_limits<size_t>::max()
                                          : static_cast<size_t>(std::stoull(maxPayload));
        }
        catch (const std::exception&)
        {
            error = "timeout_ms, retries and max_payload must be numbers";
            return nullptr;
        }
        try
        {
            destination->concurrency = std::stoi(settingOr("concurrency", "4"));
        }
        catch (const std::exception&)
        {
            error = "concurrency must be a number";
            return nullptr;
        }

        if (destination->concurrency < 1 || destination->concurrency > 64)
        {
            error = "concurrency must be between 1 and 64";
            return nullptr;
        }

        if (const auto sources = trimmed(settingOr("sources", "Send messages"));
            sources == "Send messages")
        {
            destination->sendMessages = true;
        }
        else if (sources == "Send log")
        {
            destination->sendLog = true;
        }
        else if (sources == "Send log and messages")
        {
            destination->sendMessages = true;
            destination->sendLog = true;
        }
        else
        {
            error = "sources must be Send log, Send messages or Send log and messages";
            return nullptr;
        }
        if (!parseTopicFilters(setting("topic_filter", "#"), destination->topicFilters, error))
        {
            return nullptr;
        }
        if (!parseLevel(settingOr("log_min_level", "error"), destination->minLevel))
        {
            error = "log_min_level must be debug, info, warning or error";
            return nullptr;
        }
        destination->logSubjects = splitList(setting("log_subjects"), ',');
        return destination;
    }

    bool configure()
    {
        std::string error;
        auto        destinations = configuredDestination(error);
        if (destinations == nullptr)
        {
            log(XMQ_LOG_ERROR, error);
            return false;
        }
        const std::vector<std::shared_ptr<Destination>> wanted {std::move(destinations)};

        // The host is asked once, with the filters this destination names. A list rather than the
        // widest single filter: with "#" the broker would copy every body here for nothing, and
        // the list costs one comparison per filter on the publish path instead.
        bool                     anyMessages = false;
        bool                     anyLog = false;
        size_t                   maxPayload = 0;
        uint32_t                 minLevel = XMQ_LOG_ERROR;
        std::vector<std::string> messageFilters;
        for (const auto& destination: wanted)
        {
            if (destination->sendMessages)
            {
                anyMessages = true;
                for (const auto& filter: destination->topicFilters)
                {
                    if (std::ranges::find(messageFilters, filter) == messageFilters.end())
                    {
                        messageFilters.push_back(filter);
                    }
                }
                if (destination->maxPayload == std::numeric_limits<size_t>::max())
                {
                    maxPayload = 0;
                }
                else if (maxPayload != 0)
                {
                    maxPayload = std::max(maxPayload, destination->maxPayload);
                }
            }
            if (destination->sendLog)
            {
                anyLog = true;
                minLevel = std::min(minLevel, destination->minLevel);
            }
        }

        // Named once at start: a destination that silently ignores everything looks exactly like
        // one that was never configured, and the filter is the usual reason.
        for (const auto& destination: wanted)
        {
            std::string filters;
            for (const auto& filter: destination->topicFilters)
            {
                filters += (filters.empty() ? "'" : ", '") + filter + "'";
            }
            log(XMQ_LOG_INFO, destination->name + ": " + destination->url.m_host + ":" +
                                  destination->url.m_port + destination->url.m_path +
                                  (destination->sendMessages ? " messages " + filters : "") +
                                  (destination->sendLog ? " log at " + levelName(destination->minLevel) : ""));
        }

        stopDestinations();
        {
            const std::scoped_lock lock(m_destinationsLock);
            m_current = wanted;
        }
        for (const auto& destination: wanted)
        {
            for (int at = 0; at < destination->concurrency; ++at)
            {
                destination->threads.emplace_back([destination]
                                                  {
                                                      run(destination);
                                                  });
            }
        }

        // Registered after the destinations are running: a body that arrived first would be queued
        // against a list that is about to be replaced.
        if (anyLog && !requestLogs(static_cast<xmq_log_priority>(minLevel)))
        {
            log(XMQ_LOG_ERROR, "this broker cannot deliver log records to an extension");
            stopDestinations();
            return false;
        }
        if (anyMessages && !subscribeMessages(messageFilters, maxPayload))
        {
            log(XMQ_LOG_ERROR, "this broker refused the message tap");
            stopDestinations();
            return false;
        }
        log(XMQ_LOG_INFO, std::format("sending to {} destination(s)", wanted.size()));
        return true;
    }

    mutable std::mutex                        m_destinationsLock;
    std::vector<std::shared_ptr<Destination>> m_current;
    std::atomic<uint64_t>                     m_sequence {0};
};

constexpr xmq_setting settings[] = {
    {.name = "url", .label = "URL", .description = "Where accepted messages are posted, http:// or https://", .type = XMQ_SETTING_STRING, .default_value = "", .pattern = "https?://\\S+"},
    {.name = "template", .label = "Body template", .description = "Body to post: {{topic}}, {{message}}, {{level}} and the other names are "
                                                                  "substituted and JSON-escaped. For a message {{message}} is the base64 "
                                                                  "payload and {{level}} is \"message\"; for a log record {{topic}} is empty",
     .type = XMQ_SETTING_STRING,
     .default_value = defaultTemplate},
    {.name = "headers", .label = "Headers", .description = "Extra request headers, one Name: value per item; empty means none", .type = XMQ_SETTING_LIST, .default_value = "", .pattern = "[-A-Za-z0-9!#$%&'*+.^_`|~]+: *\\S.*"},
    {.name = "secret", .label = "Secret", .description = "HMAC-SHA256 key for the X-XMQ-Signature header", .type = XMQ_SETTING_SECRET, .default_value = ""},
    {.name = "topic_filter", .label = "Topic filters", .description = "One or more MQTT filters; a message matching any is sent. + and # must each "
                                                                      "be a whole level, and # the last one",
     .type = XMQ_SETTING_LIST,
     .default_value = "#",
     .pattern = R"((?:(?:[^+#/]*|\+)/)*(?:[^+#/]*|\+|#))"},
    {.name = "max_payload", .label = "Max payload", .description = "Largest body to send, in bytes; 0 means no limit", .type = XMQ_SETTING_INTEGER, .default_value = "1048576", .pattern = "[0-9]+"},
    {.name = "timeout_ms", .label = "Timeout", .description = "HTTP connect and read timeout, in milliseconds", .type = XMQ_SETTING_INTEGER, .default_value = "5000", .pattern = "[1-9][0-9]{0,5}"},
    {.name = "retries", .label = "Retries", .description = "Attempts after the first one, 0 to 100", .type = XMQ_SETTING_INTEGER, .default_value = "3", .pattern = "[0-9]|[1-9][0-9]|100"},
    {.name = "allow_http", .label = "Allow HTTP", .description = "Permit a plain HTTP URL (for a local receiver)", .type = XMQ_SETTING_BOOLEAN, .default_value = "false"},
    {.name = "sources", .label = "Sources", .description = "What is posted: the messages the broker accepts, its own log records, or "
                                                           "both. To send neither, disable the extension",
     .type = XMQ_SETTING_CHOICE,
     .default_value = "Send messages",
     .choices = "Send log,Send messages,Send log and messages"},
    {.name = "log_min_level", .label = "Log level", .description = "Lowest log level to send", .type = XMQ_SETTING_CHOICE, .default_value = "error", .choices = "debug,info,warning,error"},
    {.name = "log_subjects", .label = "Log subjects", .description = "Comma-separated broker log subjects, for example publish,connect; empty "
                                                                     "means all",
     .type = XMQ_SETTING_STRING,
     .default_value = "",
     .pattern = "[a-z_]+(?:,[a-z_]+)*"},
    {.name = "concurrency", .label = "Concurrency", .description = "How many requests may be in flight to this destination, 1 to 64", .type = XMQ_SETTING_INTEGER, .default_value = "4", .pattern = "[1-9]|[1-5][0-9]|6[0-4]"},
};

} // namespace

XMQ_DEFINE_EXTENSION_DESCRIBED(Webhook, "webhook", "1.2", XMQ_CAP_MESSAGE_TAP | XMQ_CAP_LOG_TAP,
                               "Posts accepted MQTT messages and the broker's log to one or more "
                               "HTTP(S) endpoints.",
                               settings)
