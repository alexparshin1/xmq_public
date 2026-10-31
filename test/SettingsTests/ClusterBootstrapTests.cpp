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

#include "common/DirectoryNames.h"
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
        "web_service": {"listener_port": 0, "certfile": "/tmp/node.crt", "keyfile": "/tmp/node.key"},
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

    // The file is the bootstrap now. What the row holds is not in it any more - a copy that says
    // one thing beside a broker running another is worse than no copy at all - and what the file
    // still decides is still there.
    {
        Buffer trimmed;
        trimmed.loadFromFile(configurationPath);
        const String text(trimmed.c_str(), trimmed.bytes());

        EXPECT_EQ(String::npos, text.find("\"queue_limits\"")) << text.c_str();
        EXPECT_EQ(String::npos, text.find("\"server_limits\""));
        EXPECT_EQ(String::npos, text.find("\"logging\""));
        EXPECT_EQ(String::npos, text.find("\"this_node\""));
        EXPECT_EQ(String::npos, text.find("\"lease_seconds\""));

        EXPECT_NE(String::npos, text.find("\"connections\"")) << "the listeners are the file's to decide";
        EXPECT_NE(String::npos, text.find("\"web_service\""));
        EXPECT_NE(String::npos, text.find("\"persistence\""));
        EXPECT_NE(String::npos, text.find("\"database_uri\"")) << "the database has to stay reachable from the file";
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

/**
 * The web interface is the row's too: the port the Settings screen edits, and whether it is served
 * over TLS. The key and certificate paths stay in the file - they name files on this machine.
 */
TEST(XMQ_ClusterBootstrapTests, theWebInterfaceIsReadFromTheNodeRowAndWrittenBackToIt)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_web_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    std::string text(configurationText().c_str());
    const std::string from = "\"listener_port\": 0";
    const auto at = text.find(from);
    ASSERT_NE(std::string::npos, at) << "the fixture's web_service section has changed shape";
    text.replace(at, from.size(), "\"listener_port\": 18888, \"encrypted\": true");
    Buffer(text).saveToFile(configurationPath);

    String nodeId;
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        nodeId = cluster::NodeIdentity::load(filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName));

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_webPort.has_value()) << "the row was created without the web interface's port";
        EXPECT_EQ(18888, *row->m_webPort);
        ASSERT_TRUE(row->m_webEncrypted.has_value());
        EXPECT_TRUE(*row->m_webEncrypted);
    }

    {
        Buffer trimmed;
        trimmed.loadFromFile(configurationPath);
        const String written(trimmed.c_str(), trimmed.bytes());
        EXPECT_EQ(String::npos, written.find("\"listener_port\"")) << "the file still carries the web port";
        EXPECT_NE(String::npos, written.find("\"certfile\"")) << "the key paths are the machine's and stay";
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CWebService web;
        web.m_listener_port = 18889;
        web.m_encrypted = false;
        settings.serviceControl("set", web);

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_webPort.has_value());
        EXPECT_EQ(18889, *row->m_webPort) << "the interface did not write the row";
        ASSERT_TRUE(row->m_webEncrypted.has_value());
        EXPECT_FALSE(*row->m_webEncrypted);
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(18889, settings.m_web_service.m_listener_port.asInteger())
            << "a restart did not take the web port from the row";
        EXPECT_FALSE(settings.m_web_service.m_encrypted.asBool());
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
 * A StartupSettings file written before the listeners moved to the row still has them, and the
 * start that upgrades the node is the last one that can take them: the same start trims the file.
 * Losing them here would take a node's TLS listener away without a word.
 */
TEST(XMQ_ClusterBootstrapTests, listenersFromAnOlderStartupFileAreKept)
{
    const auto directory = filesystem::temp_directory_path() / ("xmq_legacy_listeners_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto path = directory / "xmq_server.conf";
    // The shape an earlier release wrote: no queue_limits, no logging, but the listeners still in
    // the connections section beside the keys.
    Buffer(R"({
        "connections": {
            "listener": [
                {"id": 1, "name": "Not Encrypted", "bind_ip": "0.0.0.0", "port": 18886, "threads": 2, "protocol": "MQTT", "enable": true},
                {"id": 2, "name": "Encrypted", "bind_ip": "0.0.0.0", "port": 18887, "threads": 2, "protocol": "MQTT+SSL", "enable": true}
            ],
            "ssl_keys": {"cafile": "", "keyfile": "/tmp/node.key", "certfile": "/tmp/node.crt", "verify_depth": 0}
        },
        "authentication": {"allow_anonymous": false},
        "persistence": {"redis_uri": "redis://localhost:6379", "enabled": false},
        "web_service": {"listener_port": 0},
        "bridges": [],
        "cluster": {}
    })").saveToFile(path);

    Settings settings;
    settings.loadConfiguration(path, directory / "xmq_users.conf");

    ASSERT_EQ(2U, settings.m_connections.m_listener.size())
        << "the listeners an older StartupSettings file still carries were dropped";
    EXPECT_EQ(18886, settings.m_connections.m_listener[0].m_port.asInteger());
    EXPECT_EQ(18887, settings.m_connections.m_listener[1].m_port.asInteger());

    filesystem::remove_all(directory);
}

/**
 * The listeners are the row's: their ports are what the interface changes and what the next start
 * has to read, and the file keeps only the keys beside them.
 */
TEST(XMQ_ClusterBootstrapTests, listenersAreReadFromTheNodeRowAndWrittenBackToIt)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_listeners_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    Buffer(configurationText()).saveToFile(configurationPath);

    String nodeId;
    int64_t listenerId = 0;
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        nodeId = cluster::NodeIdentity::load(filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName));
        ASSERT_FALSE(settings.m_connections.m_listener.empty());
        listenerId = settings.m_connections.m_listener[0].m_id.asInt64();

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_listeners.has_value()) << "the row was created without the listeners the file had";
        EXPECT_NE(String::npos, row->m_listeners->find("18886")) << row->m_listeners->c_str();
    }

    {
        Buffer trimmed;
        trimmed.loadFromFile(configurationPath);
        const String text(trimmed.c_str(), trimmed.bytes());
        EXPECT_EQ(String::npos, text.find("\"listener\"")) << "the file still carries the listeners";
        EXPECT_NE(String::npos, text.find("\"ssl_keys\"")) << "the keys are the machine's and stay in the file";
    }

    // A change made through the interface goes to the row, and the next start reads it there.
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CListener listener = settings.m_connections.m_listener[0];
        listener.m_port = 18887;
        settings.listenerControl("modify", listener);

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_listeners.has_value());
        EXPECT_NE(String::npos, row->m_listeners->find("18887")) << "the interface did not write the row";
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        ASSERT_FALSE(settings.m_connections.m_listener.empty());
        EXPECT_EQ(18887, settings.m_connections.m_listener[0].m_port.asInteger())
            << "a restart did not take the listener from the row";
        EXPECT_EQ(listenerId, settings.m_connections.m_listener[0].m_id.asInt64());
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
 * A node with local accounts has no row anywhere, so the file is the whole configuration and
 * nothing is rewritten out of it.
 */
TEST(XMQ_ClusterBootstrapTests, aNodeWithLocalAccountsKeepsEverySectionInTheFile)
{
    const auto directory = filesystem::temp_directory_path() / ("xmq_sqlite_file_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    std::string text(configurationText().c_str());
    const std::string from = "postgresql://gtest:test#123@dbhost_pg/xmq_test";
    const auto at = text.find(from);
    ASSERT_NE(std::string::npos, at);
    text.replace(at, from.size(),
                 string(DirectoryNames::sqliteUri(directory / "xmq_users.db").c_str()));
    Buffer(text).saveToFile(configurationPath);

    Buffer before;
    before.loadFromFile(configurationPath);

    Settings settings;
    settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

    Buffer after;
    after.loadFromFile(configurationPath);
    const String written(after.c_str(), after.bytes());

    EXPECT_NE(String::npos, written.find("\"queue_limits\"")) << "a local node's file has to keep its own settings";
    EXPECT_NE(String::npos, written.find("\"logging\""));
    EXPECT_NE(String::npos, written.find("\"server_limits\""));

    filesystem::remove_all(directory);
}

/**
 * Logging follows the queue limits out of the file: the whole section is one JSON column, so a new
 * log subject does not mean a schema change. A row that has none leaves the file in charge.
 */
TEST(XMQ_ClusterBootstrapTests, loggingIsReadFromTheNodeRowAndWrittenBackToIt)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_logging_" + to_string(::getpid()));
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
        ASSERT_TRUE(row->m_logging.has_value()) << "the row was created without the logging section the file had";
        EXPECT_NE(String::npos, row->m_logging->find("INFO")) << row->m_logging->c_str();
    }

    // Changed in the row: what the next start runs with, whatever the file says.
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CLogging changed = settings.m_logging;
        changed.m_min_log_level = "DEBUG";
        changed.m_log_level.m_publish = "DEBUG";

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        row->m_logging = changed.toString(true, false);
        store.updateLogging(nodeId, row->m_logging);
    }
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(String("DEBUG"), String(settings.m_logging.m_min_log_level.asString()))
            << "the row did not decide the log level";
        EXPECT_EQ(String("DEBUG"), String(settings.m_logging.m_log_level.m_publish.asString()))
            << "the per-subject level was lost on the way through the row";
    }

    // And what the interface sets is written to the row, not only to the file.
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CLogging logging = settings.m_logging;
        logging.m_min_log_level = "WARNING";
        settings.loggingControl("set", logging);

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_logging.has_value()) << "the interface wrote the file and left the row";
        EXPECT_NE(String::npos, row->m_logging->find("WARNING")) << row->m_logging->c_str();
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(String("WARNING"), String(settings.m_logging.m_min_log_level.asString()))
            << "a restart did not take the log level from the row";
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

TEST(XMQ_ClusterBootstrapStatic, aCreatedConfigurationSerialisesItsLoggingSectionAsJson)
{
    const auto directory = filesystem::temp_directory_path() / ("xmq_logging_text_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);
    const auto configurationPath = directory / "xmq_server.conf";

    Settings::createConfiguration(configurationPath, directory / "missing-template");
    Settings settings;
    settings.loadConfiguration(configurationPath);

    // The text the node's row carries, for a configuration the interface created rather than one
    // written by hand. It has to be JSON and it has to be text: MySQL refuses a value that is
    // neither, and PostgreSQL stores it, which is how a broken section can go unnoticed.
    EXPECT_FALSE(settings.m_logging.isNull());
    const auto text = settings.m_logging.toString(true, false);
    EXPECT_FALSE(text.empty());

    xdoc::Document document;
    EXPECT_NO_THROW(document.load(String(text))) << "the section is not JSON: " << String(text).substr(0, 120);
}

/**
 * The two ways a node meets these settings.
 *
 * Starting with a configuration that already exists: the row was written by an older version that
 * stored nothing for server_limits, so the columns are NULL and the file still decides. That is
 * what makes the upgrade safe - a node is not silently re-limited by whatever a default would have
 * been at the time the column was added.
 *
 * Creating a configuration: Initial Setup writes the file and restarts the broker, the row is
 * created from that file, and from then on the row decides. What the interface changes afterwards
 * is written to the row, not only to the file.
 */
TEST(XMQ_ClusterBootstrapTests, serverLimitsFollowTheFileUntilTheRowIsCreatedFromIt)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_server_limits_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    const auto writeConfiguration = [&](const char* maxSize, const char* maxTopicAlias)
    {
        std::string text(configurationText().c_str());
        const auto replaceText = [&text](const std::string& from, const std::string& to)
        {
            const auto at = text.find(from);
            ASSERT_NE(std::string::npos, at) << from;
            text.replace(at, from.size(), to);
        };
        replaceText("\"max_size\": 128", std::string("\"max_size\": ") + maxSize);
        replaceText("\"max_topic_alias\": 8", std::string("\"max_topic_alias\": ") + maxTopicAlias);
        Buffer(text).saveToFile(configurationPath);
    };

    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        store.createSchema();
    }

    const auto removeRow = [](const String& nodeId)
    {
        DatabaseConnectionPool       connection(pgUri, 1);
        const AutoDatabaseConnection open(connection);
        Query                        remove(open.connection(), "DELETE FROM xmq_node WHERE node_id = :node_id");
        remove.param("node_id") = nodeId;
        remove.exec();
    };

    writeConfiguration("128", "8");

    String nodeId;
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        nodeId = cluster::NodeIdentity::load(filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName));

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_maxTopicAlias.has_value()) << "the row was created without the server limits the file had";
        EXPECT_EQ(8, *row->m_maxTopicAlias);
        ASSERT_TRUE(row->m_sendThreads.has_value());
        EXPECT_EQ(1, *row->m_sendThreads) << "0 would mean auto; the file said one";
    }

    // A row written before the columns existed: nothing stored, so the file still decides - and it
    // says something different from what the row was first created with.
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        row->m_sendThreads.reset();
        row->m_receiveThreads.reset();
        row->m_deliveryThreads.reset();
        row->m_maxTopicAlias.reset();
        row->m_maxPacketSize.reset();
        row->m_queueMaxSize.reset();
        row->m_queueMaxInflight.reset();
        row->m_queueMaxBytes.reset();
        store.updateNode(*row);
    }

    writeConfiguration("999", "77");
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(999, settings.m_queue_limits.m_max_size.asInteger())
            << "a row from an older version overrode the file instead of leaving it in charge";
        EXPECT_EQ(77, settings.m_server_limits.m_max_topic_alias.asInteger());
    }

    // Creating a new configuration - Initial Setup writes the file and the broker restarts onto it:
    // the row is created from that file and becomes the configuration. The file is written whole
    // again here because the one on disk is the trimmed form: the settings it used to carry are the
    // row's, and the row just went.
    removeRow(nodeId);
    writeConfiguration("999", "77");
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value()) << "the start did not create the row for the new configuration";
        ASSERT_TRUE(row->m_maxTopicAlias.has_value());
        EXPECT_EQ(77, *row->m_maxTopicAlias);
        EXPECT_EQ(999, *row->m_queueMaxSize);
    }

    // And what the interface sets goes to the row, for both sections at once.
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");

        CServerLimits serverLimits = settings.m_server_limits;
        serverLimits.m_send_threads = "2";
        serverLimits.m_max_topic_alias = 55;
        CQueueLimits queueLimits = settings.m_queue_limits;
        queueLimits.m_max_size = 55;
        settings.limitsControl("set", serverLimits, queueLimits);

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value());
        ASSERT_TRUE(row->m_maxTopicAlias.has_value());
        EXPECT_EQ(55, *row->m_maxTopicAlias);
        ASSERT_TRUE(row->m_sendThreads.has_value());
        EXPECT_EQ(2, *row->m_sendThreads);
        EXPECT_EQ(55, *row->m_queueMaxSize);
    }

    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        EXPECT_EQ(55, settings.m_server_limits.m_max_topic_alias.asInteger()) << "a restart did not take the limits from the row";
        EXPECT_EQ(String("2"), String(settings.m_server_limits.m_send_threads.asString()));
        EXPECT_EQ(55, settings.m_queue_limits.m_max_size.asInteger());
    }

    removeRow(nodeId);
    filesystem::remove_all(directory);
}

/**
 * Resetting the configuration resets what the node runs with, and part of that is in the database.
 *
 * The row is retired before the file is replaced. Left behind, the next start with the same
 * database would find it and put the storage, the cluster switches and now the limits and logging
 * back - a reset that undoes itself while the file says defaults.
 */
TEST(XMQ_ClusterBootstrapTests, resettingTheConfigurationRetiresTheNodesRow)
{
    if (!pgAnswers())
    {
        GTEST_SKIP() << "PostgreSQL does not answer here";
    }

    const auto directory = filesystem::temp_directory_path() / ("xmq_reset_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    Buffer(configurationText()).saveToFile(configurationPath);

    String nodeId;
    {
        Settings settings;
        settings.loadConfiguration(configurationPath, directory / "xmq_users.conf");
        nodeId = cluster::NodeIdentity::load(filesystem::path(configurationPath).replace_filename(cluster::NodeIdentity::FileName));

        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        ASSERT_TRUE(store.node(nodeId).has_value()) << "the first start did not create the node's row";
    }

    String outcome;
    ASSERT_NO_THROW(outcome = Settings::resetConfiguration(configurationPath, directory / "missing-template"));
    EXPECT_NE(String::npos, outcome.find("was retired")) << outcome.c_str();

    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(pgUri, 1));
        const auto row = store.node(nodeId);
        ASSERT_TRUE(row.has_value()) << "the row was deleted rather than retired";
        EXPECT_EQ(String("Removed"), row->m_state) << "the row still claims the node's old configuration";
    }

    // The file is the one that was just created, and the one it replaced is beside it.
    EXPECT_TRUE(filesystem::exists(configurationPath));
    EXPECT_TRUE(filesystem::exists(filesystem::path(configurationPath.string() + ".old")));

    // And the node cannot quietly come back onto its old row: pointed at the same database again,
    // it refuses with the reason rather than resurrecting what was reset.
    Settings restarted;
    restarted.loadConfiguration(configurationPath, directory / "xmq_users.conf");
    restarted.setAccountDatabaseUri(pgUri);
    restarted.loadConfiguration(configurationPath, directory / "xmq_users.conf");
    EXPECT_FALSE(restarted.clusterConfigurationError().empty())
        << "a retired configuration was accepted as the node's own";

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
 * The other side of the same coin: a database that cannot be reached stops the reset instead of
 * replacing the file and leaving the row to win on the next start.
 */
TEST(XMQ_ClusterBootstrapTests, aResetThatCannotRetireTheRowChangesNothing)
{
    const auto directory = filesystem::temp_directory_path() / ("xmq_reset_unreachable_" + to_string(::getpid()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);

    const auto configurationPath = directory / "xmq_server.conf";
    std::string text(configurationText().c_str());
    const std::string from = "postgresql://gtest:test#123@dbhost_pg/xmq_test";
    const auto at = text.find(from);
    ASSERT_NE(std::string::npos, at);
    text.replace(at, from.size(), "postgresql://gtest:test#123@no-such-host.invalid/xmq_test");
    Buffer(text).saveToFile(configurationPath);

    Buffer before;
    before.loadFromFile(configurationPath);

    EXPECT_THROW(Settings::resetConfiguration(configurationPath, directory / "missing-template"), Exception);

    Buffer after;
    after.loadFromFile(configurationPath);
    EXPECT_EQ(String(before.c_str(), before.bytes()), String(after.c_str(), after.bytes()))
        << "the file was replaced even though the row could not be retired";
    EXPECT_FALSE(filesystem::exists(filesystem::path(configurationPath.string() + ".old")));

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
