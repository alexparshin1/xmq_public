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

#include "ClientSessionThread.h"
#include "base/ProtocolException.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;

namespace xmq {

ClientSessionThread::ClientSessionThread(Server* server, LogEngine& logEngine, const size_t sessionBatch)
    : m_server(server)
    , m_sessionBatch(sessionBatch < 1 ? size_t {1} : sessionBatch)
    , m_logger(logEngine)
{
    initializeSynchronized();
}

void ClientSessionThread::initializeSynchronized()
{
    const std::scoped_lock lock(m_mutex);
    m_sessionThread = thread(&ClientSessionThread::threadFunction, this);
}

ClientSessionThread::~ClientSessionThread()
{
    terminate();
    if (m_sessionThread.joinable())
    {
        m_sessionThread.join();
    }
}

size_t ClientSessionThread::sendSessionBatchSize()
{
    static const size_t batchSize = []
    {
        const char* value = getenv("XMQ_SEND_BATCH");
        const auto  parsed = value != nullptr ? strtoul(value, nullptr, 10) : DefaultSessionBatch;
        return parsed < 1 ? size_t {1} : min<size_t>(parsed, 256);
    }();
    return batchSize;
}

void ClientSessionThread::threadFunction()
{
    vector<SClientSession> batch;
    batch.reserve(m_sessionBatch);

    while (!terminated())
    {
        // One lock for the whole round: this form waits for the first session and then drains
        // whatever else is already queued, where taking them one at a time re-acquired the
        // queue's mutex for each. An idle worker still blocks exactly as it did before.
        if (m_sessionQueue.pop_front(batch, m_sessionBatch, 1s) && !terminated())
        {
            processBatch(batch);
        }
    }
}

void ClientSessionThread::processBatch(span<SClientSession> sessions)
{
    for (auto& clientSession: sessions)
    {
        if (terminated())
        {
            break;
        }
        runSession(clientSession);
    }
}

void ClientSessionThread::runSession(SClientSession& clientSession)
{
    if (!clientSession || !clientSession->isConnected())
    {
        return;
    }
    // The connection this send is for. A session that is not clean outlives its connections, so by
    // the time a write fails the client may already have reconnected and the session may be holding
    // a different one. isConnected() cannot tell the difference: it would be true of the
    // replacement, and closing on that answer drops the socket the reconnect is using - the client
    // keeps a connection the broker never reads again, and whatever it sends next (a bridge sends
    // SUBSCRIBE) is silently never dispatched.
    const auto sendConnection = clientSession->getConnection();

    try
    {
        processSession(clientSession);
    }
    // Through the server, not straight into the session, and naming the connection.
    //
    // Naming it, because comparing here and then calling the unguarded close leaves a gap between
    // the two that a takeover fits into, and the close would then drop the socket the reconnect is
    // using. Server::closeSession() repeats the comparison under the session's lock, where the
    // takeover cannot slip past it.
    //
    // Through the server, because the session only knows how to close the socket. Taking a
    // connection down also means removing it from the reactor first, and only the server can do
    // that. Closing the descriptor while it is still registered frees the number for the next
    // accept() to hand out, and the EPOLL_CTL_DEL that follows then unwatches whichever connection
    // inherited it - leaving a live client the broker never reads again.
    catch (const ConnectionException&)
    {
        m_server->closeSession(clientSession, false, sendConnection);
    }
    catch (const Exception& e)
    {
        if (clientSession->isConnected() && clientSession->getConnection() == sendConnection)
        {
            log(LogPriority::Error, "(" + m_server->getSettings()->m_cluster.m_this_node.m_node_name.asString() + ") " +
                                        string(clientSession->getClientId()) +
                                        " can't send message: " + String(e.message()));
        }
        m_server->closeSession(clientSession, false, sendConnection);
    }
}

void ClientSessionThread::terminate()
{
    const std::scoped_lock lock(m_mutex);
    m_terminated = true;
    // NOTE: intentionally do NOT call m_sessionQueue.wakeup() here. wakeup() only signals the
    // queue's semaphore without enqueuing an item; the BlockingConcurrentQueue::wait_dequeue_timed
    // then busy-spins forever in `while (!try_dequeue(item)) continue;` because no item ever
    // materialises. At shutdown that trapped every session worker in a 100% CPU spin, saturating
    // the machine and starving the join()s of the other thread pools (delivery, Web GUI), so the
    // server never finished stopping.
    //
    // An empty session instead: a real item, so the waiting pop_front() returns at once, sees
    // m_terminated and leaves without processing it. Waiting out the 1-second timeout instead cost
    // up to a second per server stop - a second of nearly every test that starts a server.
    m_sessionQueue.push_back(SClientSession {});
}

void ClientSessionThread::log(const LogPriority priority, const String& message) const
{
    m_logger.log(priority, message);
}

void ClientSessionThread::clear()
{
    m_sessionQueue.clear();
}

} // namespace xmq
