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

namespace xmq {

class ClientSession;

class XMQ_EXPORT ServerConnectionExt : public sptk::ServerConnection
{
public:
    using ServerConnection::ServerConnection;
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
    std::atomic_bool               m_isHangup {false}; ///< True if this connection hung up.
    std::atomic_bool               m_closing {false};  ///< Set by whichever thread claims the teardown.
};
} // namespace xmq
