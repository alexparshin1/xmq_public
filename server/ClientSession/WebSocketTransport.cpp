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

#include "WebSocketTransport.h"

#include <sptk5/Base64.h>
#include <sptk5/net/HttpReader.h>
#include <sptk5/wsdl/protocol/WSWebSocketsProtocol.h>

#include <sstream>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The subprotocol MQTT over WebSocket is registered under (MQTT 3.1.1/5, section 6).
constexpr const char* mqttSubprotocol = "mqtt";

/// Reads the HTTP request into a header map, or says why it is not one.
///
/// The checks are the ones RFC 6455 section 4.2.1 makes of a server: a GET on HTTP/1.1 or later, a
/// Host, an Upgrade naming websocket, a Connection carrying the upgrade token, a key that decodes
/// to the sixteen bytes the specification asks for, and version 13.
bool parseUpgradeRequest(const String& request, HttpHeaders& headers, String& failure)
{
    istringstream stream(request);
    string        firstLine;
    if (!getline(stream, firstLine))
    {
        failure = "the request has no request line";
        return false;
    }
    if (!firstLine.empty() && firstLine.back() == '\r')
    {
        firstLine.pop_back();
    }

    istringstream requestLine(firstLine);
    string        method;
    string        target;
    string        version;
    requestLine >> method >> target >> version;
    if (method != "GET")
    {
        failure = "the upgrade request is not a GET";
        return false;
    }
    if (version != "HTTP/1.1")
    {
        failure = "the upgrade request is not HTTP/1.1";
        return false;
    }

    string line;
    while (getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            break;
        }
        const auto colon = line.find(':');
        if (colon == string::npos)
        {
            failure = "a header line has no colon";
            return false;
        }
        String name(line.substr(0, colon));
        String value(line.substr(colon + 1));
        headers[name.trim()] = value.trim();
    }

    const auto header = [&headers](const char* name) -> String
    {
        const auto found = headers.find(name);
        return found == headers.end() ? String() : found->second;
    };

    /// Case-insensitive membership of a comma-separated header value, as Upgrade and Connection are.
    const auto hasToken = [](const String& value, const char* token) -> bool
    {
        for (const auto& item: String(value).split(","))
        {
            if (item.trim().toLowerCase() == token)
            {
                return true;
            }
        }
        return false;
    };

    if (header("Host").empty())
    {
        failure = "the upgrade request has no Host header";
        return false;
    }
    if (!hasToken(header("Upgrade"), "websocket"))
    {
        failure = "the Upgrade header does not name websocket";
        return false;
    }
    if (!hasToken(header("Connection"), "upgrade"))
    {
        failure = "the Connection header does not carry the upgrade token";
        return false;
    }
    if (header("Sec-WebSocket-Version").trim() != "13")
    {
        failure = "the request is not for WebSocket version 13";
        return false;
    }

    const auto key = header("Sec-WebSocket-Key").trim();
    if (key.empty())
    {
        failure = "the upgrade request has no Sec-WebSocket-Key";
        return false;
    }
    Buffer decodedKey;
    if (Base64::decode(decodedKey, key) != 16)
    {
        failure = "the Sec-WebSocket-Key does not decode to sixteen bytes";
        return false;
    }

    return true;
}

/// Builds the reply that refuses an upgrade, in the shape a browser or a client library expects.
void appendRefusal(Buffer& reply, int code, const String& reason, const String& detail)
{
    const String body = "<html><head><title>" + reason + "</title></head><body><p>" + detail +
                        "</p></body></html>\n";
    const String head = "HTTP/1.1 " + to_string(code) + " " + reason +
                        "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                        to_string(body.size()) + "\r\n\r\n";
    reply.append(head.c_str(), head.size());
    reply.append(body.c_str(), body.size());
}

} // namespace

WebSocketTransport::WebSocketTransport()
    : m_stream(WSWebSocketStream::Role::Server)
{
}

WebSocketTransport::~WebSocketTransport() = default;

WebSocketTransport::Status WebSocketTransport::receive(const uint8_t* data, size_t length, Buffer& mqtt,
                                                       Buffer& reply)
{
    const scoped_lock lock(m_mutex);

    if (m_status == Status::Closed)
    {
        return m_status;
    }

    if (m_status == Status::NeedMoreData)
    {
        // The request is a block of text, and it is only complete at the blank line that ends the
        // headers. A client may send it in pieces, and may start framing before it has read the
        // reply, so the search runs over everything received so far and the leftover after the
        // blank line is already frame bytes.
        m_request.append(data, length);

        const string_view request(m_request.c_str(), m_request.bytes());
        const auto        headerEnd = request.find("\r\n\r\n");
        if (headerEnd == string_view::npos)
        {
            if (m_request.bytes() > maxHandshakeBytes)
            {
                appendRefusal(reply, 431, "Request Header Fields Too Large",
                              "The WebSocket upgrade request is larger than this server accepts.");
                m_status = Status::Closed;
            }
            return m_status;
        }

        const auto   header = request.substr(0, headerEnd);
        const String head(header.data(), header.size());
        HttpHeaders  headers;
        String       failure;
        if (!parseUpgradeRequest(head, headers, failure))
        {
            appendRefusal(reply, 400, "Bad Request", failure);
            m_status = Status::Closed;
            return m_status;
        }

        // The key and the offered subprotocols come from the same headers the checks above read,
        // so the request is parsed once and the map is filled while it is checked.
        const auto keyHeader = headers.find("Sec-WebSocket-Key");
        const auto protocolHeader = headers.find("Sec-WebSocket-Protocol");

        m_subprotocol = protocolHeader == headers.end()
                            ? String()
                            : ws::negotiateSubprotocol(headers, {mqttSubprotocol});
        if (protocolHeader != headers.end() && m_subprotocol.empty())
        {
            // The client listed subprotocols and mqtt was not among them. Accepting anyway would
            // have it speaking something else over frames it expects to be MQTT.
            appendRefusal(reply, 400, "Bad Request",
                          "The client did not offer the mqtt WebSocket subprotocol.");
            m_status = Status::Closed;
            return m_status;
        }

        const auto response = ws::upgradeResponse(ws::acceptKey(keyHeader->second), m_subprotocol);
        reply.append(response.c_str(), response.size());

        const size_t consumed = headerEnd + 4;
        const size_t leftover = m_request.bytes() - consumed;
        m_status = Status::Open;
        if (leftover > 0)
        {
            feedFrames(m_request.data() + consumed, leftover, mqtt);
        }
        m_request.reset();
    }
    else
    {
        feedFrames(data, length, mqtt);
    }

    drainOutput(reply);

    if (m_stream.failed() || m_stream.closeReceived())
    {
        m_status = Status::Closed;
    }

    return m_status;
}

void WebSocketTransport::feedFrames(const uint8_t* data, size_t length, Buffer& mqtt)
{
    try
    {
        m_stream.feed(data, length);
    }
    catch (const Exception&)
    {
        // The stream has already queued the close frame that reports the violation, and drainOutput
        // hands it to the caller. Nothing here can be answered, so the error is not propagated: a
        // WebSocket mistake is the connection's end, not a reason to unwind the receive worker.
    }
    drainMessages(mqtt);
}

void WebSocketTransport::drainMessages(Buffer& mqtt)
{
    uint8_t chunk[8 * 1024];
    while (m_stream.available() > 0)
    {
        const auto read = m_stream.read(chunk, sizeof(chunk));
        if (read == 0)
        {
            break;
        }
        mqtt.append(chunk, read);
    }
}

void WebSocketTransport::drainOutput(Buffer& wire)
{
    if (!m_stream.hasOutput())
    {
        return;
    }
    wire.append(m_stream.output());
    m_stream.consumeOutput(m_stream.output().bytes());
}

void WebSocketTransport::send(const uint8_t* data, size_t length, Buffer& wire)
{
    const scoped_lock lock(m_mutex);
    if (m_status != Status::Open)
    {
        // Nothing queued before the upgrade was answered, and nothing after a close: either way a
        // frame written now would reach a peer that has no reason to expect it.
        return;
    }
    m_stream.write(data, length);
    drainOutput(wire);
}

void WebSocketTransport::close(Buffer& wire, uint16_t code, const String& reason)
{
    const scoped_lock lock(m_mutex);
    if (m_stream.closeSent() || m_stream.failed())
    {
        return;
    }
    m_stream.close(code, reason);
    drainOutput(wire);
    m_status = Status::Closed;
}

bool WebSocketTransport::isOpen() const
{
    const scoped_lock lock(m_mutex);
    return m_status == Status::Open;
}

bool WebSocketTransport::isClosed() const
{
    const scoped_lock lock(m_mutex);
    return m_status == Status::Closed;
}

bool WebSocketTransport::closeReceived() const
{
    const scoped_lock lock(m_mutex);
    return m_stream.closeReceived();
}

String WebSocketTransport::subprotocol() const
{
    const scoped_lock lock(m_mutex);
    return m_subprotocol;
}
