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

#include "ControlServiceTests.h"
#include "server/Cluster/NodeIdentity.h"
#include "server/OSService.h"
#include "common/DirectoryNames.h"
#include "server/Settings/ClusterStore.h"
#include "test/TestServers.h"

#include <gtest/gtest.h>
#include <sptk5/cdatabase>
#include <sptk5/net/RedisCommand.h>
#include <sptk5/net/RedisConnect.h>

#include <filesystem>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/**
 * @brief The databases a cluster can use, where a server answers.
 *
 * The same rule as the cluster store's own tests: SQLite is deliberately not among them, and a
 * database that does not answer is skipped rather than failed - a test that cannot run is not a
 * test that failed.
 */
vector<pair<String, String>> reachableClusterDatabases()
{
    vector<pair<String, String>> databases;

    vector<pair<String, String>> candidates{
        {"PostgreSQL", "postgresql://gtest:test#123@dbhost_pg/xmq_test"}};

#if !defined(_WIN32) || !defined(_DEBUG)
    candidates.emplace_back("MySQL", "mysql://gtest:test#123@dbhost_mysql/xmq_test");
#endif

    for (const auto& [name, uri]: candidates)
    {
        try
        {
            DatabaseConnectionPool       pool(uri, 1);
            const AutoDatabaseConnection connection(pool);
            Query                        probe(connection.connection(), "SELECT 1");
            probe.open();
            probe.close();
            databases.emplace_back(name, uri);
        }
        catch (const Exception&)
        {
            // No server here.
        }
    }

    return databases;
}

/// Nothing of an earlier run, so that "created" means created.
///
/// The accounts table is dropped with the cluster's own: the database is shared by every suite
/// that runs against it, and an account left there by one of them is a row this node's users file
/// then cannot migrate - the migration keeps a name that is taken, and the password of the row
/// that stayed is not the one the test signs in with. That is what made these tests fail on the
/// farm while they passed on a machine whose database happened to be clean.
void dropClusterTables(DatabaseConnectionPool& pool)
{
    for (const auto* table: {"xmq_user_to_user_group", "xmq_user_group", "xmq_user",
                             "xmq_cluster_invitation", "xmq_node"})
    {
        try
        {
            const AutoDatabaseConnection connection(pool);
            Query                        drop(connection.connection(), String("DROP TABLE ") + table);
            drop.exec();
        }
        catch (const Exception&)
        {
            // Not there, which is where this wanted to get to.
        }
    }
}

/// The accounts file a test node starts with: the suite's three accounts, as ServerTests_Suite
/// writes them for the servers it starts - the verifier form, which is the only one a password can
/// be read from.
constexpr const char* testUsers = R"({"users": [
                {
                    "id": 1,
                    "username": "admin",
                    "password": "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpYXQiOjE3MTk5MDI2MTIsImlzcyI6InNldHRpbmdzIiwiZXhwIjowLCJwYXNzd29yZCI6ImFkbWluIiwiaW5mbyI6eyJ1c2VybmFtZSI6ImFkbWluIiwiZGVwYXJ0bWVudCI6IiIsImNvbXBhbnkiOiIifX0.Ec1A-kciaOsqU95ljcaiHBBbU63hOf_Tk_C6JRxKCms",
                    "is_admin": true,
                    "is_enabled": true
                },
                {
                    "id": 2,
                    "username": "user",
                    "password": "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpYXQiOjE3MTk5MDI2MTIsImlzcyI6InNldHRpbmdzIiwiZXhwIjowLCJwYXNzd29yZCI6InNlY3JldCIsImluZm8iOnsidXNlcm5hbWUiOiJ1c2VyIiwiZGVwYXJ0bWVudCI6IiIsImNvbXBhbnkiOiIifX0.2-4mzf9XtnKDCcArX0ZvVWJ6-PaAjKzS_DMvOZASiuU",
                    "is_admin": false,
                    "is_enabled": true
                },
                {
                    "id": 3,
                    "username": "cluster",
                    "password": "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpYXQiOjE3NDkyNTA3MjEsImlzcyI6InNldHRpbmdzIiwiZXhwIjoiVDEwOjAwOjAwKzEwOjAwIiwicGFzc3dvcmQiOiJjbHVzdGVyIiwiaW5mbyI6eyJ1c2VybmFtZSI6ImNsdXN0ZXIiLCJkZXBhcnRtZW50IjoiIiwiY29tcGFueSI6IiJ9fQ.SjIBuAOTxNyjEh9OGdzF58Oy0yBqm0j7TCjca6KGRxc",
                    "is_enabled": true,
                    "is_admin": true
                }
]})";

} // namespace

/**
 * @brief The ClusterControl operation of the configuration interface, on a node whose accounts
 *        live in a database a cluster can use.
 *
 * The node is not started: every action here is about the shared database and the configuration
 * on disk, which the interface edits while the server may be stopped. What needs a running broker -
 * the invitation's travels, the link counts - is tested by the cluster suite.
 */
class XMQ_ClusterControlTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        m_directory = filesystem::temp_directory_path() /
                      ("xmq_cluster_control_" + string(testInfo->name()) + "_" + to_string(::getpid()));
        filesystem::remove_all(m_directory);
        filesystem::create_directories(m_directory);
        m_configurationPath = m_directory / "xmq_server.conf";

        const auto databases = reachableClusterDatabases();
        if (databases.empty())
        {
            GTEST_SKIP() << "No PostgreSQL or MySQL answers; a cluster database is what this tests.";
        }
        m_databaseUri = databases.front().second;

        // A row left by another suite would make "created" mean something else.
        {
            DatabaseConnectionPool pool(m_databaseUri, 1);
            dropClusterTables(pool);
        }

        const String configuration = String(R"({
            "connections": {
                "ssl_keys": {"cafile": "", "keyfile": "${ProgramCerts}/node.key", "certfile": "${ProgramCerts}/node.crt", "verify_depth": 0},
                "listener": [
                    {"id": 1, "name": "Not Encrypted", "protocol": "MQTT", "bind_ip": "0.0.0.0", "port": 13000, "threads": 2, "enable": true},
                    {"id": 2, "name": "Encrypted", "protocol": "MQTT+SSL", "bind_ip": "0.0.0.0", "port": 13800, "threads": 1, "enable": false}
                ]
            },
            "server_limits": {"max_topic_alias": 128},
            "queue_limits": {"max_size": 16384, "max_inflight_messages": 128},
            "authentication": {
                "allow_anonymous": false,
                "database_uri": ")") + m_databaseUri + R"("
            },
            "persistence": {"enabled": false, "redis_uri": "", "clean_start": true},
            "logging": {
                "log_to": "xmq_server.log",
                "min_log_level": "ERROR",
                "log_level_available": ["DEBUG","INFO","NOTICE","WARNING","ERROR","PANIC"],
                "log_level": {
                    "ack": "INFO", "connect": "INFO", "disconnect": "INFO", "publish": "DEBUG",
                    "subscribe": "DEBUG", "unsubscribe": "INFO", "server_connections": "INFO",
                    "server_events": "INFO", "cluster_connections": "INFO", "cluster_events": "INFO"
                }
            },
            "web_service": {"listener_port": 13083},
            "bridges": [],
            "cluster": {
                "this_node": {"node_name": "control-node", "host_port": "localhost:13800"},
                "nodes": []
            }
        })";

        Buffer(configuration).saveToFile(m_configurationPath);
        const auto usersPath = m_directory / "xmq_users.conf";
        Buffer(String(testUsers)).saveToFile(usersPath);

        m_settings = make_shared<Settings>();
        m_settings->setLogPriority(LogPriority::Panic);
        m_settings->loadConfiguration(m_configurationPath, usersPath);

        m_controller = make_shared<TestServerController>(nullptr, m_settings);
        m_controlService = make_shared<xmq::ControlService>(m_controller.get());
    }

    void TearDown() override
    {
        m_controlService.reset();
        m_controller.reset();
        m_settings.reset();

        if (!m_databaseUri.empty())
        {
            try
            {
                DatabaseConnectionPool pool(m_databaseUri, 1);
                dropClusterTables(pool);
            }
            catch (const Exception&)
            {
                // Leaving the tables behind is not a test failure; the next run drops them again.
            }
        }

        error_code errorCode;
        filesystem::remove_all(m_directory, errorCode);
    }

    /// The token the interface's own operations are made with, as the browser gets it.
    [[nodiscard]] String signIn() const
    {
        CLogin login;
        login.m_username = "admin";
        login.m_password = "admin";
        CLoginResponse response;
        m_controlService->Login(login, response, nullptr);
        EXPECT_TRUE(response.m_result.m_success == true) << response.m_result.m_description.asString().c_str();
        return response.m_token.asString();
    }

    /// One turn of the screen: the action, and what it came to.
    CClusterControlResponse clusterControl(const String& action, const std::function<void(CClusterControl&)>& fill = {}) const
    {
        CClusterControl input;
        input.m_action = action;
        if (fill)
        {
            fill(input);
        }

        HttpAuthentication      authentication("bearer " + signIn());
        CClusterControlResponse output;
        m_controlService->ClusterControl(input, output, &authentication);
        return output;
    }

    /// The row the test database holds for a name, or absent.
    [[nodiscard]] optional<ClusterStore::Node> storedNode(const String& name) const
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(m_databaseUri, 1));
        return store.nodeByName(name);
    }

    filesystem::path                 m_directory;
    filesystem::path                 m_configurationPath;
    String                           m_databaseUri;
    shared_ptr<Settings>             m_settings;
    shared_ptr<TestServerController> m_controller;
    shared_ptr<xmq::ControlService>  m_controlService;
};

/**
 * A node that has just started on a cluster database has a row of its own, and the screen shows
 * it as this node, standalone.
 */
TEST_F(XMQ_ClusterControlTests, aNodeOnAClusterDatabaseListsItself)
{
    const auto state = clusterControl("get");
    ASSERT_TRUE(state.m_result.m_success == true) << state.m_result.m_description.asString().c_str();

    EXPECT_TRUE(state.m_on_cluster_database == true);
    EXPECT_FALSE(state.m_enabled == true) << "a node that has never been clustered is not one";

    // The node made its own row on this start, as the first start on a cluster database does.
    ASSERT_FALSE(state.m_this_node.isNull());
    EXPECT_STREQ("control-node", state.m_this_node.m_name.getString());
    EXPECT_STREQ("Standalone", state.m_this_node.m_state.getString());
    EXPECT_TRUE(state.m_this_node.m_is_this_node == true);
    EXPECT_FALSE(state.m_this_node.m_node_id.asString().empty()) << "the row must carry the node's GUID";

    ASSERT_EQ(1U, state.m_nodes.size());
    EXPECT_STREQ("control-node", state.m_nodes[0].m_name.getString());
}

/**
 * Add declares a node: a name and an address in the shared database, with a GUID the inviter
 * generates, and nothing is contacted.
 */
TEST_F(XMQ_ClusterControlTests, addDeclaresANodeInTheSharedDatabase)
{
    const auto added = clusterControl("add", [](CClusterControl& input)
    {
        input.m_node.m_name = "node-2";
        input.m_node.m_host_port = "localhost:13801";
    });
    ASSERT_TRUE(added.m_result.m_success == true) << added.m_result.m_description.asString().c_str();

    const auto declared = storedNode("node-2");
    ASSERT_TRUE(declared.has_value());
    EXPECT_STREQ("Not Clustered", declared->m_state.c_str());
    EXPECT_STREQ("localhost:13801", declared->m_hostPort.c_str());
    EXPECT_FALSE(declared->m_nodeId.empty());
    EXPECT_TRUE(cluster::NodeIdentity::isGuid(std::string(declared->m_nodeId.c_str())))
        << "the declaring GUID must be one an invitation can be keyed against";

    ASSERT_EQ(2U, added.m_nodes.size());
    EXPECT_TRUE(added.m_nodes[1].m_is_this_node == false);
    EXPECT_FALSE(added.m_nodes[1].m_is_coordinator == true);

    // A name belongs to one node: the second declaration of it is refused, and nothing is written.
    const auto again = clusterControl("add", [](CClusterControl& input)
    {
        input.m_node.m_name = "node-2";
        input.m_node.m_host_port = "localhost:13802";
    });
    EXPECT_FALSE(again.m_result.m_success == true);
    EXPECT_NE(String::npos, again.m_result.m_description.asString().find("declared already"));

    // An address nobody could link to is refused as well.
    const auto badAddress = clusterControl("add", [](CClusterControl& input)
    {
        input.m_node.m_name = "node-3";
        input.m_node.m_host_port = "not-an-address";
    });
    EXPECT_FALSE(badAddress.m_result.m_success == true);
    EXPECT_FALSE(storedNode("node-3").has_value());
}

/**
 * Remove takes a declared node away, and refuses the node serving the interface - its own mode is
 * what takes it out of anything.
 */
TEST_F(XMQ_ClusterControlTests, removeTakesADeclaredNodeAway)
{
    ASSERT_TRUE(clusterControl("add", [](CClusterControl& input)
            {
            input.m_node.m_name = "node-2";
            input.m_node.m_host_port = "localhost:13801";
            })
        .m_result.m_success == true);

    const auto removed = clusterControl("remove", [](CClusterControl& input)
    {
        input.m_node.m_name = "node-2";
    });
    ASSERT_TRUE(removed.m_result.m_success == true) << removed.m_result.m_description.asString().c_str();
    EXPECT_FALSE(storedNode("node-2").has_value());
    EXPECT_EQ(1U, removed.m_nodes.size());

    const auto own = clusterControl("remove", [](CClusterControl& input)
    {
        input.m_node.m_name = "control-node";
    });
    EXPECT_FALSE(own.m_result.m_success == true);
    EXPECT_NE(String::npos, own.m_result.m_description.asString().find("serving this interface"));
    EXPECT_TRUE(storedNode("control-node").has_value());
}

/**
 * A member is not removed from here: it is detached, with its sessions handed over, before it is
 * removed - and that is the node's own to do.
 */
TEST_F(XMQ_ClusterControlTests, removeRefusesAMember)
{
    ASSERT_TRUE(clusterControl("add", [](CClusterControl& input)
            {
            input.m_node.m_name = "node-2";
            input.m_node.m_host_port = "localhost:13801";
            })
        .m_result.m_success == true);

    // A member, as the storage knows one: the stored state is what the check falls back on when
    // there is no coordinator to ask.
    auto member = *storedNode("node-2");
    member.m_state = "Clustered";
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(m_databaseUri, 1));
        store.createSchema();
        store.updateNode(member);
    }

    const auto refused = clusterControl("remove", [](CClusterControl& input)
    {
        input.m_node.m_name = "node-2";
    });
    EXPECT_FALSE(refused.m_result.m_success == true);
    EXPECT_NE(String::npos, refused.m_result.m_description.asString().find("member of the cluster"));
    EXPECT_TRUE(storedNode("node-2").has_value());
}

/**
 * Standalone to Cluster: the first node of a cluster. The storage is written into the node's own
 * configuration, the row says the cluster is on, and the process is asked to restart into it.
 */
TEST_F(XMQ_ClusterControlTests, enableMakesTheNodeTheFirstMemberOfACluster)
{
    TestServers::flushRedis();

    const auto enabled = clusterControl("enable", [this](CClusterControl& input)
    {
        input.m_redis_uri = TestServers::redisUri();
        input.m_database_uri = m_databaseUri;
    });
    ASSERT_TRUE(enabled.m_result.m_success == true) << enabled.m_result.m_description.asString().c_str();

    // The configuration on disk carries the accounts database as well: the bootstrap file keeps
    // the SQL URI, and the next start reads everything else from it.
    {
        Buffer configuration;
        configuration.loadFromFile(m_configurationPath);
        EXPECT_NE(String::npos, String(configuration).find(std::string(m_databaseUri.c_str())));
    }

    EXPECT_TRUE(m_settings->m_cluster.m_enabled.asBool());
    EXPECT_TRUE(enabled.m_enabled == true);

    // The configuration on disk is what the next start reads.
    Buffer configuration;
    configuration.loadFromFile(m_configurationPath);
    EXPECT_NE(String::npos, String(configuration).find(TestServers::redisUri()));

    // The previous configuration is kept: a start that cannot join goes back to it.
    EXPECT_TRUE(filesystem::exists(filesystem::path(m_configurationPath.string() + ".lastgood")));

    // The restart is how the node takes its place: the process ends with the restart code.
    EXPECT_TRUE(OSService::restartRequested());
}

/**
 * A storage that already holds a cluster is not pointed at: a cluster is joined by invitation, and
 * the answer says so. Nothing is written.
 */
TEST_F(XMQ_ClusterControlTests, enableRefusesAStorageThatHoldsAnotherCluster)
{
    TestServers::flushRedis();

    // Another cluster, with a node that is not this one.
    {
        RedisConnect redis;
        redis.connect(URL(TestServers::redisUri()));
        vector<Variant> results;
        RedisCommand    command("ZADD", "cluster:members");
        command.emplace_back("1");
        command.emplace_back(std::string_view("11111111-2222-4333-8444-555555555555"));
        redis.executeCommand(command, results);
        redis.disconnect();
    }

    const auto refused = clusterControl("enable", [this](CClusterControl& input)
    {
        input.m_redis_uri = TestServers::redisUri();
        input.m_database_uri = m_databaseUri;
    });
    EXPECT_FALSE(refused.m_result.m_success == true);
    EXPECT_NE(String::npos, refused.m_result.m_description.asString().find("by invitation"));
    EXPECT_FALSE(m_settings->m_cluster.m_enabled.asBool()) << "the mode must not change";
    EXPECT_FALSE(filesystem::exists(filesystem::path(m_configurationPath.string() + ".lastgood")))
        << "a refused enable must leave no last-good copy";
}

/**
 * An accounts database nothing answers at is refused before anything is written, as an unreachable
 * storage is.
 */
TEST_F(XMQ_ClusterControlTests, enableRefusesADatabaseThatIsNotThere)
{
    const auto refused = clusterControl("enable", [](CClusterControl& input)
    {
        input.m_redis_uri = TestServers::redisUri();
        input.m_database_uri = "postgresql://xmq:xmq@127.0.0.1:1/xmq_cluster";
    });
    EXPECT_FALSE(refused.m_result.m_success == true);
    EXPECT_FALSE(m_settings->m_cluster.m_enabled.asBool());
    EXPECT_FALSE(filesystem::exists(filesystem::path(m_configurationPath.string() + ".lastgood")));
}

/**
 * SQLite is one machine's file, and a cluster's configuration cannot live in one: the enable that
 * was given one says so rather than taking the node to a storage it could not share.
 */
TEST_F(XMQ_ClusterControlTests, enableRefusesASqliteAccountsDatabase)
{
    const auto path = filesystem::temp_directory_path() / ("xmq_cluster_control_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);

    const auto refused = clusterControl("enable", [&path](CClusterControl& input)
    {
        input.m_redis_uri = TestServers::redisUri();
        input.m_database_uri = DirectoryNames::sqliteUri(path);
    });
    EXPECT_FALSE(refused.m_result.m_success == true);
    EXPECT_NE(String::npos, refused.m_result.m_description.asString().find("SQLite is one machine's file"));
    EXPECT_FALSE(m_settings->m_cluster.m_enabled.asBool());

    filesystem::remove(path);
}

/**
 * A storage nothing answers at is refused before anything is written.
 */
TEST_F(XMQ_ClusterControlTests, enableRefusesAStorageThatIsNotThere)
{
    const auto refused = clusterControl("enable", [this](CClusterControl& input)
    {
        input.m_redis_uri = "redis://127.0.0.1:1";
        input.m_database_uri = m_databaseUri;
    });
    EXPECT_FALSE(refused.m_result.m_success == true);
    EXPECT_FALSE(m_settings->m_cluster.m_enabled.asBool());
    EXPECT_FALSE(filesystem::exists(filesystem::path(m_configurationPath.string() + ".lastgood")));
}