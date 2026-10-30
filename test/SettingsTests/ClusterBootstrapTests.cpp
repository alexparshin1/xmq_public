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
#include "server/SelfSignedCertificate.h"

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
        EXPECT_EQ(String("Standalone"), row->m_state);
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

    // An invitation is not acceptance, including after the target restarts.
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        auto row = store.node(nodeId);
        row->m_state = "Invited";
        row->m_clusterEnabled = false;
        store.updateNode(*row);
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_FALSE(settings.m_cluster.m_enabled.asBool());
        EXPECT_EQ("Invited", store.node(nodeId)->m_state);
        ASSERT_TRUE(settings.applyInvitationAccept("redis://accepted:6379", pgUri).empty());
        EXPECT_TRUE(store.node(nodeId)->m_clusterEnabled);
        EXPECT_EQ("Joining", store.node(nodeId)->m_state);
    }
    // Leaving preserves the usable SQL configuration, while removal cannot silently bootstrap.
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        ASSERT_TRUE(settings.applyClusterDisable().empty());
        ASSERT_TRUE(store.node(nodeId).has_value());
        EXPECT_FALSE(store.node(nodeId)->m_clusterEnabled);
        EXPECT_EQ("Standalone", store.node(nodeId)->m_state);
        store.removeConfiguration(nodeId);
        Settings restarted;
        restarted.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_FALSE(restarted.clusterConfigurationError().empty());
        EXPECT_EQ("Removed", store.node(nodeId)->m_state);
    }

    {
        DatabaseConnectionPool       cleanup(pgUri, 1);
        const AutoDatabaseConnection connection(cleanup);
        Query                        remove(connection.connection(), "DELETE FROM xmq_node WHERE node_id = :node_id");
        remove.param("node_id") = nodeId;
        remove.exec();
    }

    filesystem::remove_all(directory);
}

/**
 * Queue limits moved out of the file and into the node's row: a node that has them stored there
 * runs with those, whatever the file says, and the interface writes the row rather than only the
 * file. A row that predates the columns stores nothing for them and leaves the file in charge -
 * that is the whole reason they are nullable.
 */
TEST(XMQ_ClusterBootstrapTests, queueLimitsAreReadFromTheNodeRowAndWrittenBackToIt)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_queue_limits_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    Buffer(configurationText()).saveToFile(configurationPath);

    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        store.createSchema();
    }

    String nodeId;
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        nodeId = cluster::NodeIdentity::load(filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName));

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value()) << "the first start did not create the node's row";
        ASSERT_TRUE(row->m_queueMaxSize.has_value()) << "the row was created without the queue limits the file had";
        EXPECT_EQ(128, *row->m_queueMaxSize);
        ASSERT_TRUE(row->m_queueMaxInflight.has_value());
        EXPECT_EQ(16, *row->m_queueMaxInflight);
        ASSERT_TRUE(row->m_queueMaxBytes.has_value());
        EXPECT_EQ(0, *row->m_queueMaxBytes);
    }

    // What an administrator or another node changes in the row wins over the file, the same way
    // the storage URI does.
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        row->m_queueMaxSize = 64;
        row->m_queueMaxInflight = 32;
        row->m_queueMaxBytes = 4096;
        store.updateNode(*row);
    }
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(64, settings.m_queue_limits.m_max_size.asInteger()) << "the row did not decide the queue limit";
        EXPECT_EQ(32, settings.m_queue_limits.m_max_inflight_messages.asInteger());
        EXPECT_EQ(4096, settings.m_queue_limits.m_max_bytes.asInteger());
    }

    // And what the interface sets is written to the row, not only to the file.
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CQueueLimits limits;
        limits.m_max_size = 77;
        limits.m_max_inflight_messages = 33;
        limits.m_max_bytes = 65536;
        CServerLimits serverLimits = settings.m_server_limits;
        settings.limitsControl("set", serverLimits, limits);

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_queueMaxSize.has_value()) << "the interface wrote the file and left the row";
        EXPECT_EQ(77, *row->m_queueMaxSize);
        EXPECT_EQ(33, *row->m_queueMaxInflight);
        EXPECT_EQ(65536, *row->m_queueMaxBytes);
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(77, settings.m_queue_limits.m_max_size.asInteger()) << "a restart did not take the limits from the row";
        EXPECT_EQ(33, settings.m_queue_limits.m_max_inflight_messages.asInteger());
        EXPECT_EQ(65536, settings.m_queue_limits.m_max_bytes.asInteger());
    }

    {
        DatabaseConnectionPool       cleanup(pgUri, 1);
        const AutoDatabaseConnection connection(cleanup);
        Query                        remove(connection.connection(), "DELETE FROM xmq_node WHERE node_id = :node_id");
        remove.param("node_id") = nodeId;
        remove.exec();
    }

    filesystem::remove_all(directory);
}

TEST(XMQ_ClusterBootstrap, publishesTheConfiguredMqttCertificateAndRefreshesNewPeers)
{
    if (!pgAnswers()) GTEST_SKIP() << "Shared PostgreSQL test database is unavailable";
    const auto directory = filesystem::temp_directory_path() / ("xmq_certificate_test_" + cluster::NodeIdentity::generate());
    filesystem::create_directory(directory);
    const auto configuration = directory / "xmq_server.conf";
    const auto certificate = directory / "configured.crt";
    const auto key = directory / "configured.key";
    String description;
    SelfSignedCertificate::reissue(certificate, key, "configured.example.test", description);
    xdoc::Document document;
    document.load(Buffer(configurationText()));
    const auto keys = document.root()->findFirst("connections")->findFirst("ssl_keys");
    keys->set("certfile", certificate.generic_string());
    keys->set("keyfile", key.generic_string());
    const auto name = "certificate-test-" + cluster::NodeIdentity::generate();
    document.root()->findFirst("cluster")->findFirst("this_node")->set("node_name", name);
    Buffer config;
    document.exportTo(xdoc::DataFormat::JSON, config, true);
    config.saveToFile(configuration);
    Settings settings;
    settings.loadConfiguration(configuration);
    const auto store = settings.clusterStore();
    ASSERT_NE(nullptr, store);
    const auto id = settings.nodeIdentity();
    const auto peerId = cluster::NodeIdentity::generate();
    const auto peerName = "new-peer-" + peerId;
    const auto peerFile = Settings::peerCertificatesDirectory() / (peerName + ".crt");
    struct Cleanup
    {
        shared_ptr<ClusterStore> store;
        string id, peerId;
        filesystem::path directory, peerFile;
        ~Cleanup()
        {
            try { store->removeNode(peerId); store->removeNode(id); } catch (...) {}
            error_code ignored;
            filesystem::remove_all(directory, ignored);
            filesystem::remove(peerFile, ignored);
        }
    } cleanup {store, id, peerId, directory, peerFile};
    Buffer pem;
    pem.loadFromFile(certificate);
    ASSERT_TRUE(store->node(id).has_value());
    EXPECT_EQ(String(pem), store->node(id)->m_certificate);
    SelfSignedCertificate::reissue(certificate, key, "reissued.example.test", description);
    settings.loadConfiguration(configuration);
    pem.loadFromFile(certificate);
    EXPECT_EQ(String(pem), store->node(id)->m_certificate);
    ClusterStore::Node peer;
    peer.m_nodeId = peerId;
    peer.m_name = peerName;
    peer.m_hostPort = "new-peer.example.test:8883";
    peer.m_state = "Standalone";
    peer.m_databaseUri = pgUri;
    peer.m_certificate = String(pem);
    store->insertNode(peer);
    EXPECT_FALSE(filesystem::exists(peerFile));
    settings.refreshPeerCertificates();
    ASSERT_TRUE(filesystem::exists(peerFile));
    Buffer trusted;
    trusted.loadFromFile(peerFile);
    EXPECT_EQ(String(pem), String(trusted));
}
