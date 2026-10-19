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

#include "ClusterStore.h"

#include <sptk5/Exception.h>

#include <string_view>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

const char* const nodeTable = "xmq_cluster_node";
const char* const invitationTable = "xmq_cluster_invitation";

constexpr string_view nodeColumns =
    "node_id, name, host_port, state, last_error, redis_uri, database_uri,"
    " cluster_enabled, cluster_lease_seconds, cluster_links";

String nowUtc()
{
    return DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true);
}

ClusterStore::Node readNode(Query& query)
{
    ClusterStore::Node node;
    node.m_nodeId = query["node_id"].asString();
    node.m_name = query["name"].asString();
    node.m_hostPort = query["host_port"].asString();
    node.m_state = query["state"].asString();
    node.m_lastError = query["last_error"].asString();
    node.m_redisUri = query["redis_uri"].asString();
    node.m_databaseUri = query["database_uri"].asString();
    node.m_clusterEnabled = query["cluster_enabled"].asInteger() != 0;
    node.m_leaseSeconds = query["cluster_lease_seconds"].asInteger();
    node.m_links = query["cluster_links"].asInteger();
    return node;
}

} // namespace

ClusterStore::ClusterStore(shared_ptr<DatabaseConnectionPool> database)
    : m_database(std::move(database))
{
    if (!m_database)
    {
        throw Exception("ClusterStore needs a database");
    }
}

bool ClusterStore::isClusterDatabase() const
{
    // SQLite is a file on one machine. A cluster shares its configuration, so a SQLite file that
    // every node reads is either a shared disk pretending to be a database - with no locking worth
    // the name - or one node's file, and neither is a cluster.
    const auto driver = m_database->driverName().toLowerCase();
    return !driver.starts_with("sqlite");
}

String ClusterStore::tableSuffix() const
{
    const auto driver = m_database->driverName().toLowerCase();
    if (driver.starts_with("mysql") || driver.starts_with("mariadb"))
    {
        return " ENGINE=InnoDB";
    }
    return "";
}

bool ClusterStore::tableExists(const String& tableName)
{
    try
    {
        const AutoDatabaseConnection connection(*m_database);
        Query                        probe(connection.connection(), "SELECT count(*) FROM " + tableName);
        probe.open();
        probe.close();
        return true;
    }
    catch (const Exception&)
    {
        return false;
    }
}

void ClusterStore::executeStatement(const String& sql)
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(), sql);
    statement.exec();
}

void ClusterStore::createSchema()
{
    if (!tableExists(String(nodeTable)))
    {
        // One row per node, one column per setting, the GUID as the key: the shape the interface
        // edits, and the same row a node reads to learn what it is and what it joins.
        executeStatement(
            "CREATE TABLE " + String(nodeTable) + " ("
            "  node_id               varchar(36) NOT NULL,"
            "  name                  varchar(80) NOT NULL,"
            "  host_port             varchar(255) NOT NULL,"
            "  state                 varchar(24) NOT NULL,"
            "  last_error            varchar(1024),"
            "  redis_uri             varchar(1024),"
            "  database_uri          varchar(1024),"
            "  cluster_enabled       int NOT NULL DEFAULT 0,"
            "  cluster_lease_seconds int NOT NULL DEFAULT 0,"
            "  cluster_links         int NOT NULL DEFAULT 0,"
            "  updated_at            varchar(40),"
            "  CONSTRAINT pk_xmq_cluster_node PRIMARY KEY (node_id),"
            "  CONSTRAINT uq_xmq_cluster_node_name UNIQUE (name))" + tableSuffix());
    }

    if (!tableExists(String(invitationTable)))
    {
        // At most one invitation per node: a second one replaces the first, which is what the
        // primary key on node_id buys without a cleanup step.
        executeStatement(
            "CREATE TABLE " + String(invitationTable) + " ("
            "  node_id           varchar(36) NOT NULL,"
            "  token             varchar(64) NOT NULL,"
            "  inviter_host_port varchar(255) NOT NULL,"
            "  expires_at        varchar(40) NOT NULL,"
            "  CONSTRAINT pk_xmq_cluster_invitation PRIMARY KEY (node_id))" + tableSuffix());
    }
}

optional<ClusterStore::Node> ClusterStore::node(const String& nodeId)
{
    const AutoDatabaseConnection connection(*m_database);
    Query query(connection.connection(),
                "SELECT " + String(string(nodeColumns)) + " FROM " + String(nodeTable) + " WHERE node_id = :node_id");
    query.param("node_id") = nodeId;
    query.open();

    if (query.eof())
    {
        query.close();
        return nullopt;
    }

    auto result = readNode(query);
    query.close();
    return result;
}

void ClusterStore::insertNode(const Node& node)
{
    const AutoDatabaseConnection connection(*m_database);
    Query insert(connection.connection(),
                 "INSERT INTO " + String(nodeTable) +
                 " (node_id, name, host_port, state, last_error, redis_uri, database_uri,"
                 "  cluster_enabled, cluster_lease_seconds, cluster_links, updated_at)"
                 " VALUES (:node_id, :name, :host_port, :state, :last_error, :redis_uri, :database_uri,"
                 "  :cluster_enabled, :cluster_lease_seconds, :cluster_links, :updated_at)");

    insert.param("node_id") = node.m_nodeId;
    insert.param("name") = node.m_name;
    insert.param("host_port") = node.m_hostPort;
    insert.param("state") = node.m_state;
    insert.param("last_error") = node.m_lastError;
    insert.param("redis_uri") = node.m_redisUri;
    insert.param("database_uri") = node.m_databaseUri;
    insert.param("cluster_enabled") = node.m_clusterEnabled ? 1 : 0;
    insert.param("cluster_lease_seconds") = node.m_leaseSeconds;
    insert.param("cluster_links") = node.m_links;
    insert.param("updated_at") = nowUtc();
    insert.exec();
}

void ClusterStore::updateNode(const Node& node)
{
    const AutoDatabaseConnection connection(*m_database);
    Query update(connection.connection(),
                 "UPDATE " + String(nodeTable) +
                 " SET name = :name, host_port = :host_port, state = :state, last_error = :last_error,"
                 "  redis_uri = :redis_uri, database_uri = :database_uri,"
                 "  cluster_enabled = :cluster_enabled, cluster_lease_seconds = :cluster_lease_seconds,"
                 "  cluster_links = :cluster_links, updated_at = :updated_at"
                 " WHERE node_id = :node_id");

    update.param("name") = node.m_name;
    update.param("host_port") = node.m_hostPort;
    update.param("state") = node.m_state;
    update.param("last_error") = node.m_lastError;
    update.param("redis_uri") = node.m_redisUri;
    update.param("database_uri") = node.m_databaseUri;
    update.param("cluster_enabled") = node.m_clusterEnabled ? 1 : 0;
    update.param("cluster_lease_seconds") = node.m_leaseSeconds;
    update.param("cluster_links") = node.m_links;
    update.param("updated_at") = nowUtc();
    update.param("node_id") = node.m_nodeId;
    update.exec();
}

} // namespace xmq
