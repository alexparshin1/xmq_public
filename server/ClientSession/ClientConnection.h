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

#include "common/AtomicSharedPtr.h"
#include "ClientSessionThreadManager.h"
#include "PersistentClientSession.h"
#include "ServerConnectionExt.h"
#include "common/GenericProtocols.h"

#include <atomic>

namespace xmq {

class Server;
class XMQ_EXPORT ClientConnection
    : public PersistentClientSession
{
public:
    /**
     * @brief Constructor.
     * Creates the client connection object and makes it persistent
     * @param server            Server.
     * @param topicManager      Topic manager.
     * @param connectParameters Session connect parameters.
     * @param connectMessageProperties Connect message properties
     */
    explicit ClientConnection(Server*                          server,
                              const STopicManager&             topicManager,
                              const SConnectMessageParameters& connectParameters = std::make_shared<ConnectMessageParameters>(),
                              const SMessageProperties&        connectMessageProperties = {});

    /**
     * @brief Destructor
     */
    ~ClientConnection() override = default;

    /**
     * @brief Set the hangup state of the connection this session currently holds.
     * @remarks Does nothing when the session holds no connection - there is then nothing whose
     *          ending could be recorded.
     * @param hangup            True if the connection ended without an orderly disconnect.
     */
    void setHangup(const bool hangup) const
    {
        if (const auto connection = getConnection())
        {
            connection->setHangup(hangup);
        }
    }

    /**
     * @brief Check whether the connection this session currently holds ended abnormally.
     * @remarks Deliberately asks the *current* connection. A hangup recorded against a connection
     *          this session has already replaced says nothing about the one it is serving now.
     * @return True if the current connection hung up.
     */
    [[nodiscard]] bool isHangup() const
    {
        const auto connection = getConnection();
        return connection && connection->isHangup();
    }

    void clearConnection();
    void setConnection(const std::shared_ptr<ServerConnectionExt>& connection, const GenericProtocols& protocols);

    /**
     * @brief The connection this session is currently serving, as an owning pointer.
     *
     * A copy, taken atomically, and it has to be both. Three threads read this - the reactor
     * dispatching an event, the receive worker, the send worker - while a takeover replaces it and
     * a close clears it. A plain shared_ptr read racing one of those writes is not a stale answer
     * but a torn reference count: the connection can be released while a reader is still using it,
     * and the crash then lands wherever that reader happened to be.
     */
    [[nodiscard]] std::shared_ptr<ServerConnectionExt> getConnection() const
    {
        return m_connection.load(std::memory_order_acquire);
    }

    [[nodiscard]] const SClientSessionSendThread& clientSessionSendThread() const
    {
        return m_clientSessionThreads.m_sendThread;
    }

    [[nodiscard]] const SClientSessionReceiveThread& clientSessionReceiveThread() const
    {
        return m_clientSessionThreads.m_receiveThread;
    }

private:
    ClientSessionThreads m_clientSessionThreads; ///< Assigned client connection thread.
    const STopicManager  m_topicManager;         ///< Topic manager.

    /// Server connection; replaced on takeover and cleared on close, read by three threads meanwhile.
    AtomicSharedPtr<ServerConnectionExt> m_connection;
};


} // namespace xmq
