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

const char* const nodeTable = "xmq_node";
const char* const invitationTable = "xmq_cluster_invitation";

constexpr string_view nodeColumns =
    "node_id, name, host_port, state, last_error, redis_uri, database_uri,"
    " cluster_enabled, cluster_lease_seconds, cluster_links, certificate,"
    " queue_max_size, queue_max_inflight, queue_max_bytes, logging_json,"
    " limits_send_threads, limits_receive_threads, limits_delivery_threads,"
    " limits_max_topic_alias, limits_max_packet_size";

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
    node.m_certificate = query["certificate"].asString();
    if (!query["queue_max_size"].isNull()) node.m_queueMaxSize = static_cast<int>(query["queue_max_size"].asInteger());
    if (!query["queue_max_inflight"].isNull()) node.m_queueMaxInflight = static_cast<int>(query["queue_max_inflight"].asInteger());
    if (!query["queue_max_bytes"].isNull()) node.m_queueMaxBytes = query["queue_max_bytes"].asInteger();
    if (!query["logging_json"].isNull()) node.m_logging = query["logging_json"].asString();
    if (!query["limits_send_threads"].isNull()) node.m_sendThreads = static_cast<int>(query["limits_send_threads"].asInteger());
    if (!query["limits_receive_threads"].isNull()) node.m_receiveThreads = static_cast<int>(query["limits_receive_threads"].asInteger());
    if (!query["limits_delivery_threads"].isNull()) node.m_deliveryThreads = static_cast<int>(query["limits_delivery_threads"].asInteger());
    if (!query["limits_max_topic_alias"].isNull()) node.m_maxTopicAlias = static_cast<int>(query["limits_max_topic_alias"].asInteger());
    if (!query["limits_max_packet_size"].isNull()) node.m_maxPacketSize = query["limits_max_packet_size"].asInteger();
    return node;
}

/// Binds the queue-limit columns for a write, as SQL NULL when the row has no opinion: the
/// difference between "not stored" and "no limit" is the whole reason the columns are nullable.
/// The null carries the column's own type, because a parameter of no type at all is refused by
/// the PostgreSQL driver - "Unsupported parameter type 0" - rather than bound as NULL.
void bindQueueLimits(Query& query, const ClusterStore::Node& node)
{
    auto& size = query.param("queue_max_size");
    if (node.m_queueMaxSize) size = static_cast<int32_t>(*node.m_queueMaxSize);
    else size.setNull(VariantDataType::VAR_INT);

    auto& inflight = query.param("queue_max_inflight");
    if (node.m_queueMaxInflight) inflight = static_cast<int32_t>(*node.m_queueMaxInflight);
    else inflight.setNull(VariantDataType::VAR_INT);

    auto& bytes = query.param("queue_max_bytes");
    if (node.m_queueMaxBytes) bytes = static_cast<int64_t>(*node.m_queueMaxBytes);
    else bytes.setNull(VariantDataType::VAR_INT64);



    const auto bindInt = [&query](const char* name, const std::optional<int>& value) {
        auto& parameter = query.param(name);
        if (value) parameter = static_cast<int32_t>(*value);
        else parameter.setNull(VariantDataType::VAR_INT);
    };
    bindInt("limits_send_threads", node.m_sendThreads);
    bindInt("limits_receive_threads", node.m_receiveThreads);
    bindInt("limits_delivery_threads", node.m_deliveryThreads);

    auto& alias = query.param("limits_max_topic_alias");
    if (node.m_maxTopicAlias) alias = static_cast<int32_t>(*node.m_maxTopicAlias);
    else alias.setNull(VariantDataType::VAR_INT);

    auto& packet = query.param("limits_max_packet_size");
    if (node.m_maxPacketSize) packet = static_cast<int64_t>(*node.m_maxPacketSize);
    else packet.setNull(VariantDataType::VAR_INT64);
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
    if (tableExists(String(nodeTable)))
    {
        // The name xmq_node belonged to the bridge-era node table first, and shared databases
        // still hold it: id, node_state, node_name, settings. Nothing reads it any more, and its
        // shape is not this one's - so it is dropped, asked by its own columns, and the new table
        // takes the name. A probe that fails for any other reason falls through to the same drop:
        // the schema is rebuilt at every start either way.
        try
        {
            const AutoDatabaseConnection connection(*m_database);
            Query                        probe(connection.connection(),
                                               "SELECT node_id, name, host_port, state FROM " + String(nodeTable) + " LIMIT 1");
            probe.open();
            probe.close();
        }
        catch (const Exception&)
        {
            // CASCADE on PostgreSQL: the bridge era left a function taking the table's row type,
            // and a table with dependents is not dropped without it. MySQL drops with neither.
            const auto driver = m_database->driverName().toLowerCase();
            const String cascade = driver.starts_with("postgres") ? " CASCADE" : "";
            executeStatement("DROP TABLE IF EXISTS " + String(nodeTable) + cascade);
            executeStatement(String("DROP TABLE IF EXISTS xmq_node_state") + cascade);
        }
    }
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
            "  certificate           TEXT,"
            "  queue_max_size        int,"
            "  queue_max_inflight    int,"
            "  queue_max_bytes       bigint,"
            "  logging_json          TEXT,"
            "  limits_send_threads   int,"
            "  limits_receive_threads int,"
            "  limits_delivery_threads int,"
            "  limits_max_topic_alias int,"
            "  limits_max_packet_size bigint,"
            "  updated_at            varchar(40),"
            "  CONSTRAINT pk_xmq_node PRIMARY KEY (node_id),"
            "  CONSTRAINT uq_xmq_node_name UNIQUE (name))" + tableSuffix());
    }

    // Added in XMQ 1.1: each node stores its PEM certificate so other nodes can trust it
    // without a manual upload. A column that already exists makes the ALTER TABLE throw, and
    // the exception is caught here -- the schema is already current.
    try {
        const AutoDatabaseConnection connection(*m_database);
        Query alter(connection.connection(),
                    "ALTER TABLE " + String(nodeTable) + " ADD COLUMN certificate TEXT");
        alter.exec();
    } catch (const Exception&) {
        // Column already exists.
    }

    // Added with the queue limits moving out of the configuration file. Nullable and without a
    // default on purpose: a row written before this holds no opinion, and the file decides for it.
    // A default would silently overwrite the limits of every node that is already installed.
    for (const auto* column: {"queue_max_size int", "queue_max_inflight int", "queue_max_bytes bigint", "logging_json text",
                                    "limits_send_threads int", "limits_receive_threads int",
                                    "limits_delivery_threads int", "limits_max_topic_alias int",
                                    "limits_max_packet_size bigint"})
    {
        try {
            const AutoDatabaseConnection connection(*m_database);
            Query alter(connection.connection(), "ALTER TABLE " + String(nodeTable) + " ADD COLUMN " + column);
            alter.exec();
        } catch (const Exception&) {
            // Column already exists.
        }
    }

    if (!tableExists(String(invitationTable)))
    {
        // At most one invitation per node: a second one replaces the first, which is what the
        // primary key on node_id buys without a cleanup step.
        executeStatement(
            "CREATE TABLE " + String(invitationTable) + " ("
            "  node_id           varchar(36) NOT NULL,"
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

optional<ClusterStore::Node> ClusterStore::nodeByName(const String& name)
{
    const AutoDatabaseConnection connection(*m_database);
    Query query(connection.connection(),
                "SELECT " + String(string(nodeColumns)) + " FROM " + String(nodeTable) + " WHERE name = :name");
    query.param("name") = name;
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

void ClusterStore::updateLogging(const String& nodeId, const std::optional<String>& logging)
{
    const AutoDatabaseConnection connection(*m_database);
    Query update(connection.connection(),
                 "UPDATE " + String(nodeTable) + " SET logging_json = :logging_json WHERE node_id = :node_id");
    auto& parameter = update.param("logging_json");
    if (logging) parameter = *logging;
    else parameter.setNull(VariantDataType::VAR_TEXT);
    update.param("node_id") = nodeId;
    update.exec();
}

void ClusterStore::insertNode(const Node& node)
{
    const AutoDatabaseConnection connection(*m_database);
    Query insert(connection.connection(),
                 "INSERT INTO " + String(nodeTable) +
                 " (node_id, name, host_port, state, last_error, redis_uri, database_uri,"
                 "  cluster_enabled, cluster_lease_seconds, cluster_links, certificate,"
                 "  queue_max_size, queue_max_inflight, queue_max_bytes, logging_json,"
                 "  limits_send_threads, limits_receive_threads, limits_delivery_threads,"
                 "  limits_max_topic_alias, limits_max_packet_size, updated_at)"
                 " VALUES (:node_id, :name, :host_port, :state, :last_error, :redis_uri, :database_uri,"
                 "  :cluster_enabled, :cluster_lease_seconds, :cluster_links, :certificate,"
                 "  :queue_max_size, :queue_max_inflight, :queue_max_bytes, :logging_json,"
                 "  :limits_send_threads, :limits_receive_threads, :limits_delivery_threads,"
                 "  :limits_max_topic_alias, :limits_max_packet_size, :updated_at)");

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
    insert.param("certificate") = node.m_certificate;
    bindQueueLimits(insert, node);
    auto& logging = insert.param("logging_json");
    if (node.m_logging) logging = *node.m_logging;
    else logging.setNull(VariantDataType::VAR_TEXT);
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
                 "  cluster_links = :cluster_links, certificate = :certificate,"
                 "  queue_max_size = :queue_max_size, queue_max_inflight = :queue_max_inflight,"
                 "  queue_max_bytes = :queue_max_bytes,"
                 "  limits_send_threads = :limits_send_threads,"
                 "  limits_receive_threads = :limits_receive_threads,"
                 "  limits_delivery_threads = :limits_delivery_threads,"
                 "  limits_max_topic_alias = :limits_max_topic_alias,"
                 "  limits_max_packet_size = :limits_max_packet_size,"
                 "  updated_at = :updated_at"
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
    update.param("certificate") = node.m_certificate;
    bindQueueLimits(update, node);
    update.param("updated_at") = nowUtc();
    update.param("node_id") = node.m_nodeId;
    update.exec();
}

void ClusterStore::removeNode(const String& nodeId)
{
    // The invitation goes with the node: a declared node with an invitation outstanding is not a
    // member, so Remove is what the interface offers it, and the invitation must not outlive it.
    clearInvitation(nodeId);

    const AutoDatabaseConnection connection(*m_database);
    Query                        remove(connection.connection(), "DELETE FROM " + String(nodeTable) + " WHERE node_id = :node_id");
    remove.param("node_id") = nodeId;
    remove.exec();
}

void ClusterStore::removeConfiguration(const String& nodeId)
{
    auto existing = node(nodeId);
    if (!existing)
    {
        throw Exception("No such node configuration.");
    }
    auto removed = *existing;
    removed.m_name = "removed-" + nodeId;
    removed.m_hostPort = "";
    removed.m_state = "Removed";
    removed.m_lastError = "Configuration explicitly removed by an administrator.";
    removed.m_clusterEnabled = false;
    removed.m_leaseSeconds = 0;
    removed.m_links = 0;
    removed.m_redisUri = "";
    removed.m_databaseUri = "";
    removed.m_certificate = "";
    updateNode(removed); // Tombstone first: a failure must not allow silent recreation.
    clearInvitation(nodeId);
}

vector<ClusterStore::Node> ClusterStore::nodes()
{
    const AutoDatabaseConnection connection(*m_database);
    Query query(connection.connection(),
                "SELECT " + String(string(nodeColumns)) + " FROM " + String(nodeTable) + " WHERE state <> 'Removed' ORDER BY name");
    query.open();

    vector<Node> result;
    while (!query.eof())
    {
        result.push_back(readNode(query));
        query.next();
    }
    query.close();
    return result;
}

pair<String, String> ClusterStore::stateOf(const String& nodeId)
{
    const AutoDatabaseConnection connection(*m_database);
    Query query(connection.connection(),
                "SELECT state, last_error FROM " + String(nodeTable) + " WHERE node_id = :node_id");
    query.param("node_id") = nodeId;
    query.open();

    if (query.eof())
    {
        query.close();
        return {};
    }

    auto result = make_pair(query["state"].asString(), query["last_error"].asString());
    query.close();
    return result;
}

optional<ClusterStore::Invitation> ClusterStore::invitation(const String& nodeId)
{
    const AutoDatabaseConnection connection(*m_database);
    Query query(connection.connection(),
                "SELECT node_id, inviter_host_port, expires_at FROM " + String(invitationTable)
                    + " WHERE node_id = :node_id");
    query.param("node_id") = nodeId;
    query.open();

    if (query.eof())
    {
        query.close();
        return nullopt;
    }

    Invitation result;
    result.m_nodeId = query["node_id"].asString();
    result.m_inviterHostPort = query["inviter_host_port"].asString();
    result.m_expiresAt = query["expires_at"].asString();
    query.close();
    return result;
}

void ClusterStore::setInvitation(const Invitation& invitation)
{
    // Replaced rather than refused as a duplicate: a second invite is the administrator saying
    // the first one did not arrive, and the one that did arrive first must not answer to a token
    // that no longer exists. Delete-then-insert reads the same on every driver, where an upsert
    // is a different sentence on each.
    clearInvitation(invitation.m_nodeId);

    const AutoDatabaseConnection connection(*m_database);
    Query insert(connection.connection(),
                 "INSERT INTO " + String(invitationTable) +
                 " (node_id, inviter_host_port, expires_at)"
                 " VALUES (:node_id, :inviter_host_port, :expires_at)");
    insert.param("node_id") = invitation.m_nodeId;
    insert.param("inviter_host_port") = invitation.m_inviterHostPort;
    insert.param("expires_at") = invitation.m_expiresAt;
    insert.exec();
}

void ClusterStore::clearInvitation(const String& nodeId)
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        remove(connection.connection(), "DELETE FROM " + String(invitationTable) + " WHERE node_id = :node_id");
    remove.param("node_id") = nodeId;
    remove.exec();
}

} // namespace xmq
