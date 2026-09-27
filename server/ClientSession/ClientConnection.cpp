/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "ClientConnection.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;
using namespace xmq;

ClientConnection::ClientConnection(Server*                          server,
                                   const STopicManager&             topicManager,
                                   const SConnectMessageParameters& connectParameters,
                                   const SMessageProperties&        connectMessageProperties)
    : PersistentClientSession(server, connectParameters, connectMessageProperties)
    , m_clientSessionThreads(server->getClientSessionThreads())
    , m_topicManager(topicManager)
{
}

void ClientConnection::clearConnection()
{
    // No lock of its own: ClientSession::closeSession() already holds m_mutex when it calls this,
    // and that mutex is not recursive. What the lock cannot do is keep the readers out - they take
    // none - so the connection is released with an atomic store rather than a bare reset().
    clearProtocol();
    setSocket(nullptr);
    m_connection.store(nullptr, std::memory_order_release);
}

void ClientConnection::setConnection(const shared_ptr<ServerConnectionExt>& connection, const GenericProtocols& protocols)
{
    // Read before the lock is taken: getProtocolVersion() takes the same, non-recursive mutex.
    const auto& protocol = protocols.getProtocol(getProtocolVersion());
    const auto  clientSession = dynamic_pointer_cast<ClientSession>(shared_from_this());

    shared_ptr<ServerConnectionExt> supersededConnection;

    {
        // Under the session's own lock, and it has to be: every guarded close compares the
        // connection it was asked to close against the session's current one and then closes that
        // session's socket, all under this mutex. Swapping the two here without it makes the
        // comparison meaningless - a close can pass its check against the old connection and then
        // close the socket this line has just installed, which is the reconnect's own socket. The
        // client is left holding a connection the broker never reads again, and the SUBSCRIBE that
        // follows its CONNECT is never answered.
        const unique_lock lock(m_mutex);

        setProtocol(protocol);
        if (clientSession)
        {
            // Before the new socket is attached: whatever the previous connection left half-read
            // belongs to a stream that has ended, and prepending it to the new one misframes
            // everything after it.
            clientSession->discardReadBuffer();
        }
        setSocket(connection->getSocket());

        // One step, and it yields the connection being replaced. Nothing else ever lets go of that
        // one: unwatchConnection() is only ever reached through the session's *current* connection,
        // and authenticate() skips its takeover close when the old connection has already dropped -
        // which is exactly the case here, since a client that vanished rather than disconnecting is
        // why it is being replaced.
        supersededConnection = m_connection.exchange(connection, std::memory_order_acq_rel);
    }

    connection->setClientSession(clientSession);

    if (supersededConnection && supersededConnection != connection)
    {
        // Cut the back-pointer to this session. Events for a connection that is still registered
        // in the reactor resolve their session through it, so leaving it set lets the dead
        // connection's hangup tear down the socket that replaced it - and the client is left with
        // a connection the broker never reads again.
        supersededConnection->setClientSession(nullptr);
    }
}
