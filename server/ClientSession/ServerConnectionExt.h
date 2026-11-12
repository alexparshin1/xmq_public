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

#include <sptk5/net/ServerConnection.h>

#include "WebSocketTransport.h"

namespace xmq {

class ClientSession;

/**
 * @brief The listener type a configured protocol names.
 *
 * A WebSocket listener is a TCP or a TLS one underneath: the upgrade and the frames are the broker's
 * own work, not the socket's. Saying so in the type keeps the encryption question in one place -
 * ServerConnection::isEncrypted() - instead of adding a second dimension to every caller that only
 * wanted to know whether keys are needed.
 *
 * @throws sptk::Exception for a protocol this release does not serve, rather than opening a
 *         listener of some other kind on that port.
 */
[[nodiscard]] inline sptk::ServerConnection::Type connectionTypeForProtocol(const sptk::String& protocol)
{
    const auto name = protocol.toUpperCase();
    if (name == "MQTT")
    {
        return sptk::ServerConnection::Type::TCP;
    }
    if (name == "MQTT+SSL")
    {
        return sptk::ServerConnection::Type::SSL;
    }
    if (name == "MQTT+WS")
    {
        return sptk::ServerConnection::Type::WS;
    }
    if (name == "MQTT+WSS")
    {
        return sptk::ServerConnection::Type::WSS;
    }
    throw sptk::Exception("Listener protocol '" + protocol +
                          "' is not one of MQTT, MQTT+SSL, MQTT+WS and MQTT+WSS.");
}

class XMQ_EXPORT ServerConnectionExt : public sptk::ServerConnection
{
public:
    ServerConnectionExt(const Type type, const sockaddr_in* connectionAddress)
        : ServerConnection(type, connectionAddress)
        , m_webSocket(isWebSocketType(type) ? std::make_shared<WebSocketTransport>() : nullptr)
    {
    }

    ServerConnectionExt(sptk::FastTCPServer& server, const Type type, const sockaddr_in* connectionAddress)
        : ServerConnection(server, type, connectionAddress)
        , m_webSocket(isWebSocketType(type) ? std::make_shared<WebSocketTransport>() : nullptr)
    {
    }

    /**
     * @brief Whether a listener of this type speaks MQTT over WebSocket rather than MQTT directly.
     */
    [[nodiscard]] static constexpr bool isWebSocketType(const Type type)
    {
        return type == Type::WS || type == Type::WSS;
    }

    /// @brief Whether this connection speaks MQTT over WebSocket.
    [[nodiscard]] bool isWebSocket() const
    {
        return m_webSocket != nullptr;
    }

    /**
     * @brief The framing this connection carries, or nothing for a plain TCP or TLS connection.
     */
    [[nodiscard]] const std::shared_ptr<WebSocketTransport>& getWebSocket() const
    {
        return m_webSocket;
    }

    SClientSession getClientSession() const
    {
        return m_clientSession;
    }
    void setClientSession(const std::shared_ptr<ClientSession>& clientSession)
    {
        m_clientSession = clientSession;
    }

    /**
     * @brief Record that this connection ended abnormally.
     *
     * Held per connection rather than per session. A session that is not clean outlives its
     * connections, so a flag on the session cannot say *which* connection died - and once a
     * reconnect has handed the session a new connection, a hangup left over from the old one
     * would have the replacement torn down.
     *
     * @param hangup            True if the connection ended without an orderly disconnect.
     */
    void setHangup(const bool hangup)
    {
        m_isHangup = hangup;
    }

    /**
     * @brief Check whether this connection ended abnormally.
     * @return True if it did, which is what makes the last will publishable.
     */
    [[nodiscard]] bool isHangup() const
    {
        return m_isHangup;
    }

    /**
     * @brief Claim the right to tear this connection down, once.
     *
     * Two closes for the same connection are ordinary: a hangup seen by the reactor and a failed
     * write seen by a worker are independent events about the same socket, and both call for the
     * connection to go. They must not both act on it. Taking a connection down means removing its
     * descriptor from the reactor and then closing it, and if the second close starts while the
     * first is between those two steps, it runs epoll_ctl() against a descriptor that has already
     * been closed - and whose number the kernel may already have handed to the next accept(). The
     * connection that inherited it is then quietly unwatched, and its client is never read again.
     *
     * @return True for the first caller, which owns the teardown; false for every other, which
     *         must leave the connection alone.
     */
    [[nodiscard]] bool claimForClose()
    {
        return !m_closing.exchange(true, std::memory_order_acq_rel);
    }

private:
    std::shared_ptr<ClientSession> m_clientSession;
    /// Framing for a WebSocket listener's connection, and nothing for any other kind. Owned by the
    /// connection, because a reconnect hands the session a different connection with its own
    /// handshake and its own frame boundaries.
    std::shared_ptr<WebSocketTransport> m_webSocket;
    std::atomic_bool               m_isHangup {false}; ///< True if this connection hung up.
    std::atomic_bool               m_closing {false};  ///< Set by whichever thread claims the teardown.
};
} // namespace xmq
