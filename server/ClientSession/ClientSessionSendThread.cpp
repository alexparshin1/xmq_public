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
║  file, You can obtain one at https://mozilla.org/MPL/2.0.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "ClientSessionSendThread.h"
#include "ClientSession.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;

namespace xmq {

void ClientSessionSendThread::processSession(SClientSession& clientSession)
{
    // Taken before the send: a takeover replaces the session's write buffer, and this thread
    // would otherwise be sending from one that has just been destroyed.
    const auto socketWriteBuffer = clientSession->socketWriteBuffer();
    if (!socketWriteBuffer)
    {
        return;
    }

    const auto sent = socketWriteBuffer->send(m_exchangeBuffer);
    if (sent.m_bytes && server().systemStatistics())
    {
        server().systemStatistics()->registerSentData(sent.m_bytes, sent.m_messages, sent.m_publishes);
    }
}

/**
 * @brief Writes each queued session's bytes through its own buffer's send().
 *
 * One write per session, through the one flush both writers share: the send workers wake for
 * what an append queued into them, and the inline send path of a small publication writes the
 * socket itself. Both detach and write inside the buffer's own send mutex, so a session's
 * messages leave in the order they were appended, whoever was the writer.
 */
void ClientSessionSendThread::processBatch(span<SClientSession> sessions)
{
    while (m_exchangeBuffers.size() < sessions.size())
    {
        m_exchangeBuffers.push_back(make_unique<Buffer>());
    }

    size_t index = 0;
    for (auto& clientSession: sessions)
    {
        if (!clientSession || !clientSession->isConnected())
        {
            continue;
        }

        // Taken before the send: a takeover replaces the session's write buffer, and this thread
        // would otherwise be sending from one that has just been destroyed.
        const auto socketWriteBuffer = clientSession->socketWriteBuffer();
        if (!socketWriteBuffer)
        {
            continue;
        }

        auto& exchangeBuffer = m_exchangeBuffers[index++ % m_exchangeBuffers.size()];

        // The failure handling has to know after the flush is over: send() throws only for a
        // socket that cannot carry its bytes, and a takeover while it was on its way is not
        // at fault either way. Closed below, through the server, with the connection this
        // failure belonged to.
        SocketWriteMetrics sent;
        bool               failed = false;
        String             failureLog;
        try
        {
            sent = socketWriteBuffer->send(exchangeBuffer);
        }
        catch (const ConnectionException&)
        {
            // The peer went away. A lost peer goes quietly, anything else gets a line first.
            failed = true;
        }
        catch (const Exception& e)
        {
            failed = true;
            failureLog = e.message();
        }

        if (sent.m_bytes && server().systemStatistics())
        {
            server().systemStatistics()->registerSentData(sent.m_bytes, sent.m_messages, sent.m_publishes);
        }

        if (!failed)
        {
            continue;
        }

        try
        {
            if (clientSession->isConnected())
            {
                // Same treatment the one-at-a-time path gave: a lost peer goes quietly, anything
                // else is worth a line first. The close names the connection the attempt used,
                // like every other close in the broker does: a takeover between the write and the
                // close must not tear the replacement's socket down.
                if (!failureLog.empty())
                {
                    log(LogPriority::Error, "(" + server().getSettings()->m_cluster.m_this_node.m_node_name.asString() +
                                                ") " + string(clientSession->getClientId()) + " can't send message: " +
                                                failureLog);
                }
                clientSession->noteCloseSite(5);
                server().closeSession(clientSession, false, clientSession->getConnection());
            }
        }
        catch (const Exception& e)
        {
            // Nothing reasonable to do from here, and whatever the close wanted to say, the next
            // failure of the session says again.
            log(LogPriority::Error, "(" + server().getSettings()->m_cluster.m_this_node.m_node_name.asString() + ") " +
                                        string(clientSession->getClientId()) + " can't close after a failed send: " +
                                        string(e.message()));
        }
    }
}

} // namespace xmq
