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

#include <base/xmq.h>

#include <sptk5/Buffer.h>
#include <sptk5/String.h>
#include <sptk5/wsdl/protocol/WSWebSocketStream.h>

#include <memory>
#include <mutex>

namespace xmq {

/**
 * @brief MQTT carried over a WebSocket connection.
 *
 * A WebSocket listener accepts an HTTP request before it carries anything, and from then on the
 * bytes it carries are frames. Neither concern belongs to the MQTT session: what the session wants
 * from this connection is the same byte stream a plain TCP listener gives it, and the transport
 * belongs to the connection, not to the session - a reconnect hands the session a different
 * connection, and the framing state goes with the one that ended.
 *
 * So this class is the seam. Bytes read from the socket go in, MQTT bytes come out, and on the way
 * back MQTT bytes go in and framed bytes come out. The HTTP request is answered when enough of it
 * has arrived to answer; pings never surface at all, which is what keeps a browser keep-alive from
 * looking like a malformed MQTT packet.
 *
 * The stream under it is not itself thread-safe, and the receive worker and every send worker reach
 * this object at once, so each call takes the same lock. Framing a whole write under it also keeps
 * two writes from interleaving their frames.
 */
class XMQ_EXPORT WebSocketTransport
{
public:
    /// What the bytes just handed to receive() did to the transport.
    enum class Status
    {
        /// The upgrade request, or a frame, is still arriving.
        NeedMoreData,
        /// The connection is switched and carries MQTT.
        Open,
        /// The connection is finished, by a close frame or by a protocol violation. Nothing more
        /// is accepted, and what is queued for writing is the last thing this connection sends.
        Closed
    };

    WebSocketTransport();
    ~WebSocketTransport();

    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

    /**
     * @brief Accepts bytes read from the socket.
     *
     * Before the upgrade request is complete this consumes it; once it is, the same call switches
     * the connection and parses whatever the client pipelined behind the request. After that every
     * call parses frames.
     *
     * @param data              Bytes read from the socket.
     * @param length            How many there are.
     * @param mqtt              Appended with the MQTT bytes the frames carried, in order.
     * @param reply             Appended with everything that has to go back on the wire before the
     *                          caller reads again: the 101 reply, an HTTP refusal, a pong, an echoed
     *                          close, or the close frame that reports a protocol violation.
     * @return What the transport is now.
     */
    Status receive(const uint8_t* data, size_t length, sptk::Buffer& mqtt, sptk::Buffer& reply);

    /**
     * @brief Frames MQTT bytes for the wire.
     *
     * Appends one binary frame to @p wire. Nothing is appended once the connection is closing, so a
     * message queued behind a close is dropped rather than sent after it.
     */
    void send(const uint8_t* data, size_t length, sptk::Buffer& wire);

    /// Framed bytes waiting for the peer to be told the session is over, if it has not been yet.
    void close(sptk::Buffer& wire, uint16_t code = 1000, const sptk::String& reason = sptk::String());

    /// True once the upgrade request has been answered and frames are being exchanged.
    [[nodiscard]] bool isOpen() const;

    /// True once the connection is finished in either direction.
    [[nodiscard]] bool isClosed() const;

    /// True when the peer asked to close, as opposed to the broker closing on it.
    [[nodiscard]] bool closeReceived() const;

    /// The subprotocol the upgrade reply confirmed, or empty when there was none.
    [[nodiscard]] sptk::String subprotocol() const;

private:
    /// Largest request this accepts before answering 431. A handshake header is a few hundred
    /// bytes; anything near this is a client that is not going to become a WebSocket.
    static constexpr size_t maxHandshakeBytes = 16 * 1024;

    mutable std::mutex            m_mutex;
    Status                        m_status {Status::NeedMoreData};
    sptk::Buffer                  m_request;
    sptk::String                  m_subprotocol;
    sptk::WSWebSocketStream       m_stream;

    bool acceptUpgrade(const sptk::String& request, sptk::Buffer& reply);
    void feedFrames(const uint8_t* data, size_t length, sptk::Buffer& mqtt);
    void drainMessages(sptk::Buffer& mqtt);
    void drainOutput(sptk::Buffer& wire);
};

} // namespace xmq
