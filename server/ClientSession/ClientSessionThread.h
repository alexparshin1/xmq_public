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

#include "common/SessionThread.h"
#include "server/MessageDelivery.h"
#include "server/Settings/Settings.h"

#include <span>

#include <sptk5/cutils>

namespace xmq {

class GenericConnection;
class GenericProtocol;
class Server;

class ClientSession;
using SClientSession = std::shared_ptr<ClientSession>;

class XMQ_EXPORT ClientSessionThread
{
public:
    /**
     * @brief Constructor.
     */
    /**
     * @param server            Owning server.
     * @param logEngine         Log engine.
     * @param sessionBatch      How many sessions this worker takes from its queue per round.
     */
    ClientSessionThread(Server* server, sptk::LogEngine& logEngine, size_t sessionBatch = 1);

    /**
     * @brief Destructor.
     */
    virtual ~ClientSessionThread();

    void join()
    {
        m_sessionThread.join();
    }

    void terminate();

    bool terminated() const
    {
        return m_terminated;
    }

    /**
     * @brief Log a message.
     * @param priority          Log priority.
     * @param message           Message to log.
     */
    void log(sptk::LogPriority priority, const sptk::String& message) const;

    void queueProcessSession(const SClientSession& clientSession)
    {
        m_sessionQueue.push_back(clientSession);
    }

    size_t sessionQueueLength() const
    {
        std::scoped_lock lock(m_mutex);
        return m_sessionQueue.size();
    }

    Server& server() const
    {
        return *m_server;
    }

protected:
    virtual void processSession(SClientSession& /*clientSession*/) = 0;

    /**
     * @brief Default number of sessions a worker takes from the queue per blocking wait.
     *
     * Only the send workers take a round. The receive workers take one session at a time, and the
     * comment on sendSessionBatchSize() carries the measurement that keeps it that way.
     *
     * What a round is for: every pop_front() takes the queue's mutex, and every pop that finds the
     * queue empty parks the worker, so at a quarter of a million flushes a second both show up in
     * the profile. Taking several sessions per blocking wait pays one lock and one wakeup instead
     * of N. What it costs is ordering - a session taken into a round waits for the ones ahead of
     * it to be encoded and written instead of going to whichever worker is idle. That trade
     * suits the send side, where work arrives in backlogs, and not the receive side, where it
     * arrives a message at a time.
     *
     * The value of 3 is inherited, not re-proven. It came from a sweep this comment used to
     * reproduce, and that sweep - like every bench number taken before 2026-09-20 - was measured
     * on a rig whose NIC had a single hardware queue and no RPS, which serialised precisely the
     * parallelism a round-size sweep exists to price. The tables have been removed rather than
     * left here to be cited; git history has them if anyone wants to see what was withdrawn.
     * Nothing has shown 3 to be wrong, and nothing has shown it to be best either.
     */
    static constexpr size_t DefaultSessionBatch = 3;

    /**
     * @brief How many sessions the send workers take from the queue per blocking wait.
     *
     * The first take of a round blocks; the rest are non-blocking, so an empty queue costs exactly
     * what it did before.
     *
     * Overridden by XMQ_SEND_BATCH, which exists for experiments; 1 restores the original
     * one-at-a-time behaviour.
     *
     * The receive workers take one session at a time, and that was re-measured on 2026-09-21 on
     * the repaired bench - three arms out of one binary, switched at run time, interleaved, three
     * rounds. Read as differences between arms, not as absolutes: these are end-to-end scenario
     * medians, which on that bench carry a couple of hundred microseconds of network and a load
     * client that saturates before the broker does, so the absolute Fan-Out figure says far more
     * about the rig than about this queue. The arms differ only in the switch, which is what makes
     * the comparison worth anything.
     *
     *     receive round     Fan-Out    P2P    P2P tail   broker CPU
     *     1 (as shipped)      -         -        -       2.63 / 2.54 cores
     *     10                +0.4%     +1%      -6%       2.60 / 2.54 cores
     *
     * A round of ten changes nothing on either axis, because the queue is all but always empty
     * when a worker wakes: the batch form of pop_front() takes a single item nearly every time,
     * so there is no wakeup to amortise. The futex traffic here is structural - one handoff per
     * message - not burstiness that grouping can coalesce.
     *
     * Removing the queue altogether was measured in the same run and rejected. It does buy CPU,
     * 11% on Fan-Out and 20% on P2P, because the reactor then reads the session itself and nothing
     * is handed anywhere. But Fan-Out latency rises 73% and the P2P tail 2.5x, against 4% on the
     * P2P median - the ordering cost above, paid in full. Lower CPU does not buy latency here.
     *
     * If an absolute is ever wanted here, take it from the broker's own phase breakdown rather
     * than from a scenario median, for the reason the first paragraph gives.
     */
    static size_t sendSessionBatchSize();

    /**
     * @brief Runs one session through processSession(), with the error handling around it.
     */
    void runSession(SClientSession& clientSession);

    /**
     * @brief Handles the sessions taken from the queue in one round.
     *
     * The default runs them one at a time, which is what receive workers want. The send worker
     * overrides it: having the whole round in hand is what lets one mechanism write to many
     * sockets at once.
     */
    virtual void processBatch(std::span<SClientSession> sessions);

private:
    mutable std::mutex                      m_mutex;
    Server*                                 m_server;
    sptk::SynchronizedQueue<SClientSession> m_sessionQueue;
    const size_t                            m_sessionBatch;               ///< Sessions taken per round; 1 for receive workers.
    sptk::Logger                            m_logger;
    std::thread                             m_sessionThread;
    std::atomic_bool                        m_terminated {false};

    /**
     * @brief Clear send and receive queues.
     */
    void clear();

    void threadFunction();

    void initializeSynchronized();
};

} // namespace xmq
