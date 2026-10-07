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

#include "base/xmq.h"

#include <sptk5/net/RedisConnect.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xmq::cluster {

/**
 * @brief This node's part in the cluster's membership, coordinator and client-service lease.
 *
 * The shared Redis storage is the arbiter. Every node already depends on it to serve clients, and it
 * has the one clock all of them agree on, so a lease is a key with a TTL and nobody compares clocks:
 *
 * - cluster:members - the admitted nodes, scored in the order they were admitted: the succession
 *   order. At most MaxMembers; a node that is down still counts until it leaves.
 * - cluster:alive:<node> - the node's client-service lease, which the node renews itself every step.
 *   A node serves clients while it holds it, which is while it can reach Redis. When it expires the
 *   node is gone as far as the cluster is concerned, and its sessions may be taken over elsewhere;
 *   the node itself has stopped serving them by then.
 * - cluster:term - the coordinator generation, incremented by every node that becomes coordinator.
 * - cluster:coordinator - "<term> <node>", set only when absent and kept by renewal.
 *
 * The coordinator is not on the clients' path. It looks after membership and recovery - taking over
 * what a node that is gone served - and changing it is invisible to clients: no node's lease
 * depends on it. When the coordinator key is gone, the first member in succession order takes it at
 * once and each one after it waits one tick longer, so the most senior node that is alive becomes
 * coordinator. The coordinator is kept first in the order: a node found ahead of it - a former
 * coordinator, or one passed over while it was down - goes to the end.
 */
class XMQ_EXPORT Coordinator
{
public:
    static constexpr size_t MaxMembers = 10; ///< The supported cluster size.

    /// Called from the coordinator's thread when this node goes cluster-online or cluster-offline.
    using StateChanged = std::function<void(bool online)>;

    /**
     * @brief Constructor. Nothing happens until start().
     * @param nodeName          This node's name.
     * @param redisUrl          The shared storage, the URL the node's own storage connected to.
     * @param lease             How long a lease lasts without renewal.
     * @param stateChanged      Told when the node goes online or offline.
     */
    Coordinator(std::string nodeName, sptk::URL redisUrl, std::chrono::milliseconds lease, StateChanged stateChanged);

    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    /**
     * @brief Admit this node to the cluster, if it is not a member yet, and start taking part.
     *
     * Takes the first step at once, so the node comes out of this with its lease, and the first
     * node of a cluster as its coordinator.
     *
     * @throws sptk::Exception  The cluster has MaxMembers members already, or Redis cannot be reached.
     */
    void start();

    /**
     * @brief Admit another node, as the node it asked to join through does before linking back.
     * @param nodeName          The joining node.
     * @return False when the cluster has MaxMembers members already and the node is not one.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] bool admit(const std::string& nodeName);

    /**
     * @brief Stop taking part. Membership stays: the node is down, not gone.
     */
    void stop();

    /**
     * @brief Leave the cluster: stop, give up the coordinator key if this node holds it, and remove
     *        this node from the members, which frees its place.
     */
    void leave();

    /**
     * @brief For tests: act as if Redis could not be reached, or could again.
     * @param lost              True to fail every Redis command.
     */
    void simulateStorageLoss(bool lost);

    /**
     * @return True while this node may serve clients: it holds its lease, or is not taking part -
     *         never started, or left the cluster.
     */
    [[nodiscard]] bool isOnline() const;

    /**
     * @return True while this node holds the coordinator key.
     */
    [[nodiscard]] bool isCoordinator() const
    {
        return m_coordinator.load();
    }

    /**
     * @return The coordinator as of the last step, or empty when there was none.
     */
    [[nodiscard]] std::string coordinatorName() const;

    /**
     * @return The coordinator's term as of the last step, 0 when there was none.
     */
    [[nodiscard]] int64_t term() const
    {
        return m_term.load();
    }

    /**
     * @brief The members in succession order.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] std::vector<std::string> members();

    /**
     * @brief Delete every cluster key: membership, terms, the coordinator and the leases.
     *
     * For tests, which start a new cluster on a storage the previous one used.
     * @param redis             A connection to the storage.
     */
    static void clearClusterState(sptk::RedisConnect& redis);

private:
    using Clock = std::chrono::steady_clock;

    const std::string               m_nodeName;
    const sptk::URL                 m_redisUrl;
    const std::chrono::milliseconds m_lease;
    const std::chrono::milliseconds m_tick;
    StateChanged                    m_stateChanged;

    mutable std::mutex      m_mutex;          ///< Guards everything below that is not atomic.
    std::condition_variable m_wake;           ///< Wakes the thread for a step or to stop.
    bool                    m_stopping {false};
    std::thread             m_thread;
    sptk::RedisConnect      m_redis;          ///< The coordinator's own: the storage's is busy with async work.
    std::string             m_coordinatorName;
    Clock::time_point       m_coordinatorAbsentSince {}; ///< Zero while there is a coordinator.

    std::atomic<int64_t>           m_leaseUntil {0};   ///< Clock ticks; the lease is valid before it.
    std::atomic<bool>              m_storageLost {false}; ///< See simulateStorageLoss().
    std::atomic<bool>              m_coordinator {false};
    std::atomic<int64_t>           m_term {0};
    std::atomic<bool>              m_reportedOnline {true};
    std::atomic<bool>              m_participating {false}; ///< Between start() and leave().

    void run();
    void step();
    void connectRedis();
    std::vector<sptk::Variant> eval(const std::string& script, const std::vector<std::string>& keys,
                                    const std::vector<std::string>& arguments);
    void reportState();
};

} // namespace xmq::cluster
