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

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sptk { class TCPSocket; }

namespace xmq::exporter {

struct Url
{
    bool        m_tls{true};
    std::string m_host;
    uint16_t    m_port{};
    std::string m_path{"/"};
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

bool parseUrl(const std::string& text, Url& url);
bool parseHeaders(std::string_view text, HttpHeaders& headers, std::string& error);

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
class HttpPeer
{
public:
    HttpPeer(Url url, std::string secret, int timeoutMs, HttpHeaders headers);
    bool deliver(const std::string& body, int retries) { return deliveryStatus(body, retries) == 0; }
    int deliveryStatus(const std::string& body, int retries);

private:
    int send(const std::string& body);
    void connect();

    Url                                              m_url;
    std::string                                      m_secret;
    std::chrono::milliseconds                        m_timeout;
    HttpHeaders                                      m_headers;
    std::shared_ptr<sptk::TCPSocket>                  m_socket;
};

} // namespace xmq::exporter
