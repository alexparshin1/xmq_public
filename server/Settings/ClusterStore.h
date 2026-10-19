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

    /// Adds a row for a node that has none yet.
    void insertNode(const Node& node);

    /// Writes what an administrator changes; the row must exist.
    void updateNode(const Node& node);

    /// True when the driver is PostgreSQL or MySQL - what a cluster configuration needs.
    [[nodiscard]] bool isClusterDatabase() const;

private:
    [[nodiscard]] sptk::String tableSuffix() const;
    [[nodiscard]] bool          tableExists(const sptk::String& tableName);
    void                        executeStatement(const sptk::String& sql);

    std::shared_ptr<sptk::DatabaseConnectionPool> m_database;
};

} // namespace xmq
