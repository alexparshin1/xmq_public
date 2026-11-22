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

#include "HttpPeer.h"
#include "SqlEngine.h"

#include <sptk5/net/HttpConnect.h>
#include <sptk5/net/SSLSocket.h>
#include <sptk5/net/URL.h>

#include <openssl/hmac.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <thread>

namespace xmq::exporter {

namespace {

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

} // namespace

using namespace xmq::webhook_sql;

bool parseUrl(const std::string& text, Url& url)
{
    try
    {
        const sptk::URL parsed(text);
        const auto scheme = parsed.protocol();
        if (scheme != "http" && scheme != "https")
            return false;
        const auto authority = std::string_view(text).substr(scheme.size() + 3);
        // Credentials are not supported in endpoint URLs; use the Headers setting.
        if (authority.substr(0, authority.find_first_of("/?#")).find('@') != std::string_view::npos)
            return false;
        auto [host, port] = parsed.hostAndPort();
        if (host.empty())
            return false;
        const bool tls = scheme == "https";
        if (!port)
            port = tls ? 443 : 80;
        url = {tls, std::move(host), port, parsed.requestTarget()};
        return true;
    }
    catch (const sptk::Exception&)
    {
        return false;
    }
}

/// Extra request headers, one "Name: value" per item; a single string is accepted too.
///
/// The same shape as topic_filter: the interface edits a list, the file stores a JSON array, and a
/// value written by hand as one header still works.
bool parseHeaders(const std::string_view text, std::vector<std::pair<std::string, std::string>>& headers,
                  std::string&           error)
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

HttpPeer::HttpPeer(Url url, std::string secret, const int timeoutMs, HttpHeaders headers)
    : m_url(std::move(url))
      , m_secret(std::move(secret))
      , m_timeout(timeoutMs)
      , m_headers(std::move(headers))
{
}

int HttpPeer::send(const std::string& body)
{
    const sptk::Buffer payload{std::string_view(body)};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const bool reused = m_socket && m_socket->active();
        try
        {
            if (!reused)
                connect();
            sptk::HttpConnect http(m_socket);
            // The delivery status is all we need; don't buffer an unbounded response.
            http.setResponseLimits(64 * 1024, 1024 * 1024);
            auto& headers = http.requestHeaders();
            headers["Content-Type"] = "application/json";
            headers["User-Agent"] = "xmq-webhook/1.2";
            headers["Connection"] = "keep-alive";
            headers["Accept-Encoding"] = "identity";
            for (const auto& [name, value]: m_headers)
                headers[sptk::String(name)] = sptk::String(value);
            if (!m_secret.empty())
            {
                unsigned char digest[EVP_MAX_MD_SIZE];
                unsigned int length = 0;
                if (!HMAC(EVP_sha256(), m_secret.data(), static_cast<int>(m_secret.size()),
                          reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &length))
                    throw std::runtime_error("cannot sign HTTP request");
                headers["X-XMQ-Signature"] = sptk::String("sha256=" + hex(digest, length));
            }
            sptk::Buffer response;
            m_socket->setIoDeadline(std::chrono::steady_clock::now() + m_timeout);
            const auto status = http.cmd_post(sptk::String(m_url.m_path), {}, payload, response, {}, nullptr, m_timeout);
            m_socket->setIoDeadline(std::nullopt);
            return status;
        }
        catch (const std::exception&)
        {
            if (m_socket)
                m_socket->close();
            if (!reused)
                break;
        }
    }
    return -1;
}

void HttpPeer::connect()
{
    if (m_url.m_tls)
    {
        auto tls = std::make_shared<sptk::SSLSocket>();
        tls->configureClientVerification(sptk::String(m_url.m_host), {});
        m_socket = std::move(tls);
    }
    else
        m_socket = std::make_shared<sptk::TCPSocket>();
    m_socket->open(sptk::Host(m_url.m_host, m_url.m_port), sptk::Socket::OpenMode::CONNECT, false, m_timeout);
}

/// 2xx is success; 4xx except 429 is refused without a second attempt; everything else retries
/// with backoff. The same policy for both inputs, in one place.
int HttpPeer::deliveryStatus(const std::string& body, int retries)
{
    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        const int status = send(body);
        if (status >= 200 && status < 300)
        {
            return 0;
        }
        if (status >= 400 && status < 500 && status != 429)
        {
            // The endpoint will say no again; a retry would only be noise.
            return 1;
        }
        if (attempt < retries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(2000, 100 << std::min(attempt, 5))));
        }
    }
    return 2;
}

} // namespace xmq::exporter
