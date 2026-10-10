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
 * has the one clock all of them agree on, so a lease is a key with a TTL and nobody compares clocks.
 * A node is its GUID (see NodeIdentity); its name is for people, and unique in the cluster:
 *
 * - cluster:members - the GUIDs of the admitted nodes, scored in the order they were admitted: the
 *   succession order. At most MaxMembers; a node that is down still counts until it leaves.
 * - cluster:names - node name -> GUID. A name belongs to one node; another node may not take it.
 * - cluster:node:<guid> - the node's name and the TLS address the other nodes link to.
 * - cluster:alive:<guid> - the node's client-service lease, which the node renews itself every step.
 *   It holds the id of the node's current run, so a second process with the same GUID - a cloned
 *   machine, a node started twice - finds it taken and does not start. A node serves clients while
 *   it holds its lease, which is while it can reach Redis. When the lease expires the node is gone as
 *   far as the cluster is concerned, and its sessions may be taken over elsewhere; the node itself
 *   has stopped serving them by then.
 * - cluster:term - the coordinator generation, incremented by every node that becomes coordinator.
 * - cluster:coordinator - "<term> <guid>", set only when absent and kept by renewal.
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

    /// A member as the others see it: what to call it and where to link to it.
    struct Member
    {
        std::string m_name;     ///< Node name.
        std::string m_hostPort; ///< TLS endpoint, host:port.
        bool        m_alive;    ///< Holds its lease: it is running.
    };

    /**
     * @brief Constructor. Nothing happens until start().
     * @param nodeId            This node's GUID.
     * @param nodeName          This node's name.
     * @param hostPort          The TLS endpoint the other nodes link to.
     * @param redisUrl          The shared storage, the URL the node's own storage connected to.
     * @param lease             How long a lease lasts without renewal.
     * @param stateChanged      Told when the node goes online or offline.
     */
    Coordinator(std::string nodeId, std::string nodeName, std::string hostPort, sptk::URL redisUrl,
                std::chrono::milliseconds lease, StateChanged stateChanged);

    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    /**
     * @brief Admit this node to the cluster, if it is not a member yet, and start taking part.
     *
     * Takes the first step at once, so the node comes out of this with its lease, and the first
     * node of a cluster as its coordinator. A node restarting after a crash may find its previous
     * run's lease still there; it waits for that to expire - its previous run is serving nothing.
     *
     * @throws sptk::Exception  The cluster is full, another node has this name, this node is running
     *                          elsewhere already, or Redis cannot be reached.
     */
    void start();

    /**
     * @brief Stop taking part. Membership stays: the node is down, not gone.
     */
    void stop();

    /**
     * @brief Leave the cluster: stop, give up the coordinator key if this node holds it, and remove
     *        this node from the members, which frees its place and its name.
     */
    void leave();

    /**
     * @brief For tests: act as if Redis could not be reached, or could again.
     * @param lost              True to fail every Redis command.
     */
    void simulateStorageLoss(bool lost);

    /**
     * @brief For tests: act as if Redis stopped answering, without refusing - every command waits
     *        until this is turned off again, as one does on a network that has lost the storage.
     * @param hung              True to make every Redis command wait.
     */
    void simulateStorageHang(bool hung);

    /**
     * @return True while this node may serve clients: it holds its lease, or is not taking part -
     *         never started, or left the cluster.
     */
    [[nodiscard]] bool isOnline() const;

    /**
     * @return True between start() and leave(): the node is in a cluster.
     */
    [[nodiscard]] bool isParticipating() const
    {
        return m_participating.load();
    }

    /**
     * @return True while this node holds the coordinator key.
     */
    [[nodiscard]] bool isCoordinator() const
    {
        return m_coordinator.load();
    }

    /**
     * @return The coordinator's name as of the last step, or empty when there was none.
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
     * @return This node's GUID.
     */
    [[nodiscard]] const std::string& nodeId() const
    {
        return m_nodeId;
    }

    /**
     * @brief The members in succession order, this node included.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] std::vector<Member> members();

    /**
     * @brief Is the named node a member? A node that is not refuses links from it.
     * @param nodeName          Node name.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] bool isMember(const std::string& nodeName);

    /**
     * @brief Record a member for a node that is not running, as a test's stand-in for a node that is
     *        down: it counts towards MaxMembers like any member that is down.
     * @param nodeName          Name for the stand-in; it gets a GUID of its own.
     * @return False when the cluster is full.
     */
    [[nodiscard]] bool admitAbsentNode(const std::string& nodeName);

    /**
     * @brief Take a session for this node, if nobody else running holds it.
     *
     * session_<clientId>_owner names the node that serves the session. It is taken when it is
     * free, this node's already, or held by a node whose lease has expired - a node that is gone,
     * and serves nothing.
     *
     * @param clientId          Client id.
     * @return Empty when the session is this node's now; otherwise the GUID of the node holding it.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] std::string claimSession(const std::string& clientId);

    /**
     * @brief Hand a session this node holds to another node, which asked for it.
     * @param clientId          Client id.
     * @param nodeId            GUID of the node taking it.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    void handOverSession(const std::string& clientId, const std::string& nodeId);

    /**
     * @param nodeId            A member's GUID.
     * @return Its name, or empty for an unknown GUID.
     * @throws sptk::Exception  Redis cannot be reached.
     */
    [[nodiscard]] std::string nodeName(const std::string& nodeId);

    /**
     * @brief What the shared storage says about a node with this GUID.
     * @param redis             A connection to the storage.
     * @param nodeId            The node's GUID.
     * @return 1: a member; 0: not a member of a cluster that exists; -1: there is no cluster.
     */
    static int membership(sptk::RedisConnect& redis, const std::string& nodeId);

    /**
     * @brief Delete every cluster key: membership, names, terms, the coordinator and the leases.
     *
     * For tests, which start a new cluster on a storage the previous one used.
     * @param redis             A connection to the storage.
     */
    static void clearClusterState(sptk::RedisConnect& redis);

private:
    using Clock = std::chrono::steady_clock;

    const std::string               m_nodeId;
    const std::string               m_nodeName;
    const std::string               m_hostPort;
    const std::string               m_runId; ///< This process's run of the node.
    const sptk::URL                 m_redisUrl;
    const std::chrono::milliseconds m_lease;
    const std::chrono::milliseconds m_tick;
    StateChanged                    m_stateChanged;

    mutable std::mutex      m_mutex;          ///< Guards everything below that is not atomic.
    std::condition_variable m_wake;           ///< Wakes the thread to stop.
    bool                    m_stopping {false};
    std::thread             m_thread;
    std::mutex              m_watchMutex;     ///< Guards m_stopWatch; never held with m_mutex.
    std::condition_variable m_watchWake;      ///< Wakes the watch thread to stop.
    bool                    m_stopWatch {false};
    std::thread             m_watch;          ///< Reports going offline and online, see watch().
    sptk::RedisConnect      m_redis;          ///< The coordinator's own: the storage's is busy with async work.
    std::string             m_coordinatorName;
    Clock::time_point       m_coordinatorAbsentSince {}; ///< Zero while there is a coordinator.

    std::atomic<int64_t> m_leaseUntil {0};        ///< Clock ticks; the lease is valid before it.
    std::atomic<bool>    m_storageLost {false};   ///< See simulateStorageLoss().
    std::atomic<bool>    m_storageHung {false};   ///< See simulateStorageHang().
    std::atomic<bool>    m_coordinator {false};
    std::atomic<int64_t> m_term {0};
    std::atomic<bool>    m_reportedOnline {true};
    std::atomic<bool>    m_participating {false}; ///< Between start() and leave().

    void run();
    void watch();
    void step();
    void connectRedis();
    std::vector<sptk::Variant> eval(const std::string& script, const std::vector<std::string>& keys,
                                    const std::vector<std::string>& arguments);
    void reportState();
};

} // namespace xmq::cluster
