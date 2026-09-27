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

#include "ClientSessionSendThread.h"
#include "ClientSession.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;

namespace xmq {

void ClientSessionSendThread::processSession(SClientSession& clientSession)
{
    // Held for the duration: a takeover replaces the session's write buffer, and this thread would
    // otherwise be sending from one that has just been destroyed.
    const auto socketWriteBuffer = clientSession->socketWriteBuffer();
    if (!socketWriteBuffer)
    {
        return;
    }

    const auto sendBytes = socketWriteBuffer->bytes();
    const auto messageCount = socketWriteBuffer->messageCount();
    const auto publishCount = socketWriteBuffer->publishCount();
    socketWriteBuffer->send(m_exchangeBuffer);
    if (server().systemStatistics())
    {
        server().systemStatistics()->registerSentData(sendBytes, messageCount, publishCount);
    }
}

void ClientSessionSendThread::processBatch(span<SClientSession> sessions)
{
    while (m_exchangeBuffers.size() < sessions.size())
    {
        m_exchangeBuffers.push_back(make_unique<Buffer>());
    }

    m_writeItems.clear();
    m_writeItems.reserve(sessions.size());
    m_connectionOfItem.clear();
    m_connectionOfItem.reserve(sessions.size());

    // Detach first, write second. Everything a session had queued leaves its buffer here, so the
    // delivery threads can carry on filling the fresh one while the round is on its way out.
    size_t index = 0;
    for (auto& clientSession: sessions)
    {
        if (!clientSession || !clientSession->isConnected())
        {
            continue;
        }

        // Held for the duration: a takeover replaces the session's write buffer, and this thread
        // would otherwise be sending from one that has just been destroyed.
        const auto socketWriteBuffer = clientSession->socketWriteBuffer();
        if (!socketWriteBuffer)
        {
            continue;
        }

        const auto sendBytes = socketWriteBuffer->bytes();
        const auto messageCount = socketWriteBuffer->messageCount();
        const auto publishCount = socketWriteBuffer->publishCount();

        auto& exchangeBuffer = m_exchangeBuffers[index];
        socketWriteBuffer->detach(exchangeBuffer);
        ++index;

        if (server().systemStatistics())
        {
            server().systemStatistics()->registerSentData(sendBytes, messageCount, publishCount);
        }

        // The session may have disconnected between being queued and this round running; if so
        // there is nothing left to write to, and the detached bytes go nowhere.
        const auto socket = clientSession->getSocket();
        if (!socket || exchangeBuffer->empty())
        {
            continue;
        }

        // The socket goes in as the owning reference it already is. Taking .get() here left the
        // batch holding a pointer whose owner died at the end of this iteration, and a session
        // torn down before the round was written took the socket with it.
        m_writeItems.push_back(SocketWriteItem {.m_socket = socket,
                                                .m_data = exchangeBuffer->data(),
                                                .m_size = exchangeBuffer->bytes()});
        m_sessionOfItem.push_back(clientSession);
        m_connectionOfItem.push_back(clientSession->getConnection());
    }

    m_writer->writeAll(m_writeItems);

    for (size_t item = 0; item < m_writeItems.size(); ++item)
    {
        if (!m_writeItems[item].m_failed)
        {
            continue;
        }
        auto& clientSession = m_sessionOfItem[item];
        if (!clientSession->isConnected())
        {
            continue;
        }
        // Same treatment the one-at-a-time path gave: a lost peer goes quietly, anything else is
        // worth a line first.
        if (!m_writeItems[item].m_error.empty())
        {
            log(LogPriority::Error, "(" + server().getSettings()->m_cluster.m_this_node.m_node_name.asString() + ") " +
                                        string(clientSession->getClientId()) +
                                        " can't send message: " + m_writeItems[item].m_error);
        }
        clientSession->noteCloseSite(5);
        // Through the server and naming the connection, the way the one-at-a-time path does it.
        // Closing through the session alone leaves the connection in the reactor, and closing
        // without naming it skips the comparison every other close makes: a write that failed on a
        // connection the session has since replaced tore down the replacement's socket, and the
        // client was left believing it was connected while the broker read nothing from it. This
        // is the path a failing reconnect test names.
        server().closeSession(clientSession, false, m_connectionOfItem[item]);
    }
    m_sessionOfItem.clear();
    m_connectionOfItem.clear();
}

} // namespace xmq
