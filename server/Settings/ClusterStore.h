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

#pragma once

#include "base/xmq.h"

#include <sptk5/cdatabase>
#include <sptk5/cutils>

#include <memory>
#include <optional>

namespace xmq {

/**
 * @brief The cluster's own configuration, in the shared SQL database.
 *
 * The settings that describe a cluster rather than a machine - the storage every node works
 * against, the accounts database, the cluster's switches, and which nodes have been declared -
 * cannot live in a file that is copied from machine to machine. They are shared data, and a
 * shared SQL database is where they belong: one row per node in `xmq_cluster_node`, keyed by the node's
 * GUID, and its columns are the settings.
 *
 * Redis is not replaced by this: membership, leases, sessions and retained messages are written
 * several times per lease and stay there. What is here is what an administrator edits and what
 * has to survive a restart of every node.
 *
 * PostgreSQL and MySQL only. SQLite is not a cluster database: the file belongs to one machine,
 * and a cluster of nodes sharing it does not exist.
 */
class XMQ_EXPORT ClusterStore
{
public:
    /// One declared node, as the database holds it.
    struct Node
    {
        sptk::String m_nodeId;          ///< The node's GUID, as kept in `xmq_node.id`.
        sptk::String m_name;            ///< For people, unique in the cluster.
        sptk::String m_hostPort;        ///< The TLS address other nodes link to.
        sptk::String m_state;           ///< Not Clustered, Invited, Joining, Refused, Expired, Failed.
        sptk::String m_lastError;       ///< Why the last attempt ended as it did.
        sptk::String m_redisUri;        ///< The storage the cluster works against.
        sptk::String m_databaseUri;     ///< The accounts database every node shares.
        bool         m_clusterEnabled {false};
        int          m_leaseSeconds {0};///< 0 means the default, as in cluster.lease_seconds.
        int          m_links {0};       ///< 0 means the default, as in cluster.links.
        sptk::String m_certificate;        ///< The node's PEM certificate, for automatic trust.

        // Queue limits. Optional because a row written before these columns existed has no value
        // for them, and the configuration file is then still what decides: null means "not stored
        // here", not "no limit". A stored 0 does mean no limit, which is why the distinction
        // cannot be made with the number alone.
        std::optional<int>    m_queueMaxSize;      ///< queue_limits.max_size.
        std::optional<int>    m_queueMaxInflight;  ///< queue_limits.max_inflight_messages.
        std::optional<int64_t> m_queueMaxBytes;    ///< queue_limits.max_bytes.
    };

    /// One invitation, as the database holds it. At most one per node.
    struct Invitation
    {
        sptk::String m_nodeId;          ///< The invited node's GUID - its identity, no token needed.
        sptk::String m_inviterHostPort; ///< The address the invitation came from.
        sptk::String m_expiresAt;       ///< ISO time, ten minutes from the invite.
    };

    explicit ClusterStore(std::shared_ptr<sptk::DatabaseConnectionPool> database);

    /**
     * @brief Creates the tables if they are not there, and does nothing if they are.
     *
     * Safe at every start, as UserStore::createSchema() is: a migration step somebody has to
     * remember to run is a broker that will not start because of it.
     */
    void createSchema();

    /// The node's row, if it has one.
    [[nodiscard]] std::optional<Node> node(const sptk::String& nodeId);

    /// The row known by a name, if there is one. Names are unique in the cluster, so this is the
    /// declaration an administrator made with Add, before the node itself was asked its GUID.
    [[nodiscard]] std::optional<Node> nodeByName(const sptk::String& name);

    /// Adds a row for a node that has none yet.
    void insertNode(const Node& node);

    /// Writes what an administrator changes; the row must exist.
    void updateNode(const Node& node);

    /// Erases a provisional declaration (e.g. when Invite replaces it with the real UUID).
    void removeNode(const sptk::String& nodeId);

    /// Explicit configuration removal. Keep a tombstone so startup cannot recreate the row.
    void removeConfiguration(const sptk::String& nodeId);

    /// Every declared node, for the Cluster Configuration screen.
    [[nodiscard]] std::vector<Node> nodes();

    /// The node's stored state only, with the error text of its last attempt; both empty when
    /// there is no row. The states that come from Redis - Clustered, Offline - are not here.
    [[nodiscard]] std::pair<sptk::String, sptk::String> stateOf(const sptk::String& nodeId);

    /// The invitation a node has, if one is active.
    [[nodiscard]] std::optional<Invitation> invitation(const sptk::String& nodeId);

    /// Puts an invitation in place of the one a node had, if any - one per node, the latest wins.
    void setInvitation(const Invitation& invitation);

    /// Takes a node's invitation away: accepted, declined, or expired.
    void clearInvitation(const sptk::String& nodeId);

    /// True when the driver is PostgreSQL or MySQL - what a cluster configuration needs.
    [[nodiscard]] bool isClusterDatabase() const;

private:
    [[nodiscard]] sptk::String tableSuffix() const;
    [[nodiscard]] bool          tableExists(const sptk::String& tableName);
    void                        executeStatement(const sptk::String& sql);

    std::shared_ptr<sptk::DatabaseConnectionPool> m_database;
};

} // namespace xmq
