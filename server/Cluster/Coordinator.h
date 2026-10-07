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
 * @brief This node's part in choosing the cluster coordinator and in holding a client-service lease.
 *
 * The shared Redis storage is the arbiter. Every node already depends on it to serve clients, and it
 * has the one clock all of them agree on, so a lease is a key with a TTL and nobody compares
 * clocks:
 *
 * - cluster:members - the admitted nodes, scored in the order they were admitted: the succession
 *   order. At most MaxMembers; a node that is down still counts until it leaves.
 * - cluster:term - the coordinator generation, incremented by every node that becomes coordinator.
 * - cluster:coordinator - "<term> <node>", set only when absent and kept by renewal. A node whose
 *   renewal fails is coordinator no more; another can take the key only once it has expired.
 * - cluster:lease:<node> - "<term>", the client-service lease. Only the coordinator writes them,
 *   in the same step that renews its own key and with the same TTL, so no node lease outlives the
 *   authority that gave it.
 *
 * When the coordinator key is gone, the first member in succession order takes it at once and each
 * one after it waits one tick longer, so the most senior node that is alive becomes coordinator. The
 * coordinator is kept first in the order: a node found ahead of it - a former coordinator, or one
 * passed over while it was down - goes to the end.
 *
 * A node without a valid lease is cluster-offline: it serves no clients. So is a node that cannot
 * reach Redis, whatever its lease says. A node that has just joined has one lease period to get
 * its first lease before that applies to it.
 */
class XMQ_EXPORT Coordinator
{
public:
    static constexpr size_t MaxMembers = 10; ///< The supported cluster size.

    /// Called from the coordinator's thread when this node goes cluster-online or cluster-offline.
    using StateChanged = std::function<void(bool online)>;

    /// The names of the peers this node has a link to. The coordinator gives leases to these.
    using ConnectedPeers = std::function<std::vector<std::string>()>;

    /**
     * @brief Constructor. Nothing happens until start().
     * @param nodeName          This node's name.
     * @param redisUrl          The shared storage, the URL the node's own storage connected to.
     * @param lease             How long a lease lasts without renewal.
     * @param connectedPeers    Names of the peers this node has a link to.
     * @param stateChanged      Told when the node goes online or offline.
     */
    Coordinator(std::string nodeName, sptk::URL redisUrl, std::chrono::milliseconds lease,
                ConnectedPeers connectedPeers, StateChanged stateChanged);

    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    /**
     * @brief Admit this node to the cluster, if it is not a member yet, and start taking part.
     *
     * Takes the first step at once, so the first node of a cluster comes out of this as its
     * coordinator, with a lease.
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
     * @brief Run a step now instead of at the next tick: a peer has connected, and the coordinator
     *        can give it a lease without making it wait.
     */
    void nudge();

    /**
     * @return True while this node may serve clients: it holds a lease, or has just joined, or is
     *         not taking part - never started, or left the cluster.
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
    ConnectedPeers                  m_connectedPeers;
    StateChanged                    m_stateChanged;

    mutable std::mutex      m_mutex;          ///< Guards everything below that is not atomic.
    std::condition_variable m_wake;           ///< Wakes the thread for a step or to stop.
    bool                    m_stopping {false};
    bool                    m_nudged {false};
    std::thread             m_thread;
    sptk::RedisConnect      m_redis;          ///< The coordinator's own: the storage's is busy with async work.
    std::string             m_coordinatorName;
    Clock::time_point       m_coordinatorAbsentSince {}; ///< Zero while there is a coordinator.

    std::atomic<int64_t>           m_leaseUntil {0};   ///< Clock ticks; the lease is valid before it.
    std::atomic<int64_t>           m_graceUntil {0};   ///< Clock ticks; a joining node's first lease period.
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
