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

#include "ClientSessionReceiveThread.h"
#include "ClientSessionSendThread.h"

namespace xmq {

struct ClientSessionThreads
{
    SClientSessionReceiveThread m_receiveThread;
    SClientSessionSendThread    m_sendThread;
};

/**
 * @brief Connection thread pool.
 * @param server                Server.
 * @param threadCount           Number of threads.
 * @param publishThreads        Publish threads.
 * @param logEngine             Logger.
 */
class XMQ_EXPORT ClientSessionThreadManager final
{
public:
    /**
     * @brief Constructor.
     * @param server            Server.
     * @param sendThreadCount   Number of send threads.
     * @param receiveThreadCount Number of receive threads.
     * @param logEngine         Logger.
     */
    explicit ClientSessionThreadManager(Server* server, size_t sendThreadCount, size_t receiveThreadCount, sptk::LogEngine& logEngine);

    /**
     * @brief Destructor.
     */
    ~ClientSessionThreadManager();

    /**
     * @brief Get next session threads from the thread pool.
     * @return session threads.
     */
    ClientSessionThreads getClientSessionThreads();

    /**
     * @brief Stop the thread pool.
     */
    void stop();

    /**
     * @brief Total number of sessions waiting in the receive workers' queues.
     *
     * The one number that says the broker has started falling behind before the delay shows up in
     * latency: sessions handed over by the reactor and not yet taken. Summed across workers, so a
     * single busy worker does not hide behind idle ones.
     *
     * Reading it takes each queue's own mutex - the same one push and pop take - so this is for
     * the once-a-second metrics scan, not for anything on the message path.
     */
    [[nodiscard]] size_t receiveQueueLength() const;

    /**
     * @brief Total number of sessions waiting in the send workers' queues.
     */
    [[nodiscard]] size_t sendQueueLength() const;

private:
    mutable std::mutex                       m_mutex;                      ///< Mutex to protect access to the thread list
    std::vector<SClientSessionReceiveThread> m_receiveThreads;             ///< Session receive threads
    std::vector<SClientSessionSendThread>    m_sendThreads;                ///< Session send threads
    size_t                                   m_nextSendThreadIndex {0};    ///< Next thread index
    size_t                                   m_nextReceiveThreadIndex {0}; ///< Next thread index
};

} // namespace xmq
