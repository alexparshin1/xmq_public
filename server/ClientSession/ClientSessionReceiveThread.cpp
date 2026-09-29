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

#include "ClientSessionReceiveThread.h"
#include "ClientSession.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void ClientSessionReceiveThread::processSession(SClientSession& clientSession)
{
    // The connection this pass is about, taken before any of it runs. A reconnect can hand the
    // session a replacement at any point below - the CONNECT it carries is processed on another
    // thread - and every decision from here on has to stay about the connection whose bytes were
    // just read. Asking the session instead returns whatever it holds by the time the question is
    // put, which after a takeover is the reconnect's own connection.
    const auto processedConnection = clientSession->getConnection();

    const auto result = clientSession->receiveMessages();

    // Asked of that connection, not of the session: a hangup belongs to the connection that ended,
    // and the health of the one that replaced it says nothing about it.
    if (processedConnection && processedConnection->isHangup())
    {
        // Recorded before the close, which clears it: the last will belongs to a client that
        // vanished, and must not be published for one that has already reconnected.
        const auto stillServingThisConnection = clientSession->getConnection() == processedConnection;

        server().closeSession(clientSession, false, processedConnection);

        if (stillServingThisConnection)
        {
            clientSession->handleConnectionHangup();
        }
    }
    else
    {
        if (clientSession->isConnected())
        {
            if (clientSession->awaitingWriteCapacity())
            {
                // Neither re-armed nor queued: a OneShot re-arm would fire straight back for the
                // bytes left in the socket. Resuming queues the session.
            }
            else if (server().getTriggerMode() == SocketPoolTriggerMode::OneShot)
            {
                // OneShot disarmed the socket when the event fired; the re-arm re-checks
                // readiness, so any bytes left in the socket re-fire the event immediately.
                server().watchSession(clientSession, true);
            }
            else if (!result.noMoreMessages)
            {
                // EdgeTriggered: the socket stays armed, but leftover bytes never re-fire
                // on their own — take another pass over this session.
                queueProcessSession(clientSession);
            }
        }
    }

    if (server().systemStatistics())
    {
        server().systemStatistics()->registerReceivedData(result.bytes, result.anyMessages, result.publishMessages);
    }
}
