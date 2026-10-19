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

#include "server/Cluster/NodeIdentity.h"
#include "server/Settings/ClusterStore.h"
#include "server/Settings/Settings.h"

#include <gtest/gtest.h>
#include <sptk5/cdatabase>
#include <sptk5/cutils>

#include <filesystem>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr const char* pgUri = "postgresql://gtest:test#123@dbhost_pg/xmq_test";

bool pgAnswers()
{
    try
    {
        DatabaseConnectionPool       pool(pgUri, 1);
        const AutoDatabaseConnection connection(pool);
        Query                        probe(connection.connection(), "SELECT 1");
        probe.open();
        probe.close();
        return true;
    }
    catch (const Exception&)
    {
        return false;
    }
}

/// A configuration with the accounts database set to PostgreSQL, which is the bootstrap this test
/// is about; everything else is the least a configuration can hold.
String configurationText()
{
    return R"({
        "connections": {
            "listener": [{"id": 1, "name": "Default", "protocol": "MQTT", "bind_ip": "127.0.0.1", "port": 18886, "threads": 1, "enable": true}],
            "ssl_keys": {"cafile": "", "keyfile": "${ProgramCerts}/node.key", "certfile": "${ProgramCerts}/node.crt", "verify_depth": 0}
        },
        "server_limits": {"send_threads": 1, "receive_threads": 1, "max_topic_alias": 8},
        "queue_limits": {"max_size": 128, "max_inflight_messages": 16},
        "authentication": {"allow_anonymous": false, "database_uri": "postgresql://gtest:test#123@dbhost_pg/xmq_test"},
        "persistence": {"redis_uri": "redis://from-the-file:6379", "enabled": false},
        "logging": {"log_to": "xmq_server.log", "min_log_level": "INFO", "log_level": {
            "ack": "INFO", "connect": "INFO", "disconnect": "INFO", "publish": "INFO",
            "subscribe": "INFO", "unsubscribe": "INFO", "server_connections": "INFO",
            "server_events": "INFO", "cluster_connections": "INFO", "cluster_events": "INFO"}},
        "web_service": {"listener_port": 0},
        "bridges": [],
        "cluster": {"this_node": {"node_name": "bootstrap-node", "host_port": "localhost:8883"}}
    })";
}

} // namespace

/**
 * The node's row is the configuration: created from the file on the first start, and read instead
 * of the file from then on. What the interface edits is the row, so a setting written there must
 * be what the next start runs with.
 */
TEST(XMQ_ClusterBootstrapTests, theNodeRowIsCreatedOnceAndReadAfterwards)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_bootstrap_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    Buffer(configurationText()).saveToFile(configurationPath);

    DatabaseConnectionPool pool(pgUri, 1);
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        store.createSchema();
    }

    String nodeId;

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        // The GUID file appears beside the configuration on the first load, and the row is keyed
        // by it.
        const auto identityPath = filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName);
        ASSERT_TRUE(filesystem::exists(identityPath)) << "no node identity was written beside the configuration";
        nodeId = cluster::NodeIdentity::load(identityPath);
        ASSERT_TRUE(cluster::NodeIdentity::isGuid(nodeId)) << nodeId.c_str();

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto  row = store.node(nodeId);
        ASSERT_TRUE(row.has_value()) << "the first start did not create the node's row";
        EXPECT_EQ(String("bootstrap-node"), row->m_name) << "the row was not filled from the configuration";
        EXPECT_EQ(String("localhost:8883"), row->m_hostPort);
        EXPECT_EQ(String("redis://from-the-file:6379"), row->m_redisUri);
        EXPECT_EQ(String("Not Clustered"), row->m_state);
    }

    // What the interface would do: change the row.
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        auto        row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        row->m_redisUri = "redis://from-the-database:6379";
        row->m_clusterEnabled = true;
        row->m_name = "renamed-while-running";
        store.updateNode(*row);
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        EXPECT_EQ(String("redis://from-the-database:6379"), String(settings.m_persistence.m_redis_uri.asString()))
            << "the storage was not taken from the node's row";
        EXPECT_TRUE(settings.m_cluster.m_enabled.asBool()) << "the cluster switch was not taken from the node's row";
        EXPECT_EQ(String("renamed-while-running"), String(settings.m_cluster.m_this_node.m_node_name.asString()))
            << "the node's name was not taken from the row";
    }

    {
        DatabaseConnectionPool       cleanup(pgUri, 1);
        const AutoDatabaseConnection connection(cleanup);
        Query                        remove(connection.connection(), "DELETE FROM xmq_cluster_node WHERE node_id = :node_id");
        remove.param("node_id") = nodeId;
        remove.exec();
    }

    filesystem::remove_all(directory);
}
