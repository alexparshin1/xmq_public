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
#include "server/Settings/ClusterStore.h"

#include <gtest/gtest.h>
#include <sptk5/cdatabase>
#include <sptk5/cutils>

#include <filesystem>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The databases a cluster can use, where a server answers. SQLite is deliberately not among them:
/// the store reports it as not a cluster database, which is a test of its own.
vector<pair<String, String>> reachableClusterDatabases()
{
    vector<pair<String, String>> databases;

    vector<pair<String, String>> candidates {
        {"PostgreSQL", "postgresql://gtest:test#123@dbhost_pg/xmq_test"}};

#if !defined(_WIN32) || !defined(_DEBUG)
    // Not in a Windows debug build: MySQL's client library is release-only there, and a failed
    // connection across two C runtimes corrupts the heap instead of throwing.
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
            // No server here; a test that cannot run is not a test that failed.
        }
    }

    String covered;
    for (const auto& [name, uri]: databases)
    {
        covered += (covered.empty() ? "" : ", ") + name;
    }
    COUT("            Cluster databases reached: " << covered << endl);

    return databases;
}

/// Nothing of an earlier run, so that "created" means created.
void dropClusterTables(DatabaseConnectionPool& pool)
{
    for (const auto* table: {"xmq_cluster_invitation", "xmq_node"})
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

/// One schema per test, on every cluster database that answers, and none left behind.
class XMQ_ClusterStoreTests
    : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        m_databases = reachableClusterDatabases();
    }

    static void TearDownTestSuite()
    {
        for (const auto& [name, uri]: m_databases)
        {
            DatabaseConnectionPool pool(uri, 1);
            dropClusterTables(pool);
        }
    }

    void SetUp() override
    {
        for (const auto& [name, uri]: m_databases)
        {
            DatabaseConnectionPool pool(uri, 1);
            dropClusterTables(pool);
        }
    }

    static vector<pair<String, String>> m_databases;
};

vector<pair<String, String>> XMQ_ClusterStoreTests::m_databases;

ClusterStore::Node sampleNode(const String& nodeId)
{
    ClusterStore::Node node;
    node.m_nodeId = nodeId;
    node.m_name = "node-1";
    node.m_hostPort = "localhost:8883";
    node.m_state = "Not Clustered";
    node.m_redisUri = "redis://localhost:6379";
    node.m_databaseUri = "postgresql://db/xmq";
    node.m_clusterEnabled = true;
    node.m_leaseSeconds = 2;
    node.m_links = 4;
    node.m_queueMaxSize = 1024;
    node.m_queueMaxInflight = 128;
    node.m_queueMaxBytes = 1'048'576;
    node.m_logging = R"({"log_to":"xmq_server.log","min_log_level":"DEBUG","log_level":{"publish":"DEBUG"}})";
    return node;
}

} // namespace

TEST_F(XMQ_ClusterStoreTests, createsItsSchemaOnEveryClusterDatabaseWeSupport)
{
    for (const auto& [name, uri]: m_databases)
    {
        DatabaseConnectionPool pool(uri, 2);
        ClusterStore           store(make_shared<DatabaseConnectionPool>(uri, 2));

        EXPECT_TRUE(store.isClusterDatabase()) << name;
        EXPECT_NO_THROW(store.createSchema()) << name;

        // Called at every start, so it has to be safe when everything is already there.
        EXPECT_NO_THROW(store.createSchema()) << name << ": creating twice must be harmless";

        EXPECT_FALSE(store.node("00000000-0000-4000-8000-000000000000").has_value())
            << name << ": an unknown node must read as absent";
    }
}

TEST_F(XMQ_ClusterStoreTests, aNodeRowKeepsEverySetting)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();

        const String nodeId = "6f1c2b7e-9a3d-4c55-8e21-0b7d5a9c4e10";
        store.insertNode(sampleNode(nodeId));

        const auto stored = store.node(nodeId);
        ASSERT_TRUE(stored.has_value()) << name;
        EXPECT_EQ(nodeId, stored->m_nodeId) << name;
        EXPECT_EQ(String("node-1"), stored->m_name) << name;
        EXPECT_EQ(String("localhost:8883"), stored->m_hostPort) << name;
        EXPECT_EQ(String("Not Clustered"), stored->m_state) << name;
        EXPECT_EQ(String("redis://localhost:6379"), stored->m_redisUri) << name;
        EXPECT_EQ(String("postgresql://db/xmq"), stored->m_databaseUri) << name;
        EXPECT_TRUE(stored->m_clusterEnabled) << name;
        EXPECT_EQ(2, stored->m_leaseSeconds) << name;
        EXPECT_EQ(4, stored->m_links) << name;
        ASSERT_TRUE(stored->m_queueMaxSize.has_value()) << name;
        EXPECT_EQ(1024, *stored->m_queueMaxSize) << name;
        ASSERT_TRUE(stored->m_queueMaxInflight.has_value()) << name;
        EXPECT_EQ(128, *stored->m_queueMaxInflight) << name;
        ASSERT_TRUE(stored->m_queueMaxBytes.has_value()) << name;
        EXPECT_EQ(1'048'576, *stored->m_queueMaxBytes) << name;
        ASSERT_TRUE(stored->m_logging.has_value()) << name;
        EXPECT_NE(String::npos, stored->m_logging->find("DEBUG")) << name << ": the logging section was not stored";

        // What the interface edits is written back through the same row.
        auto changed = *stored;
        changed.m_name = "node-1-renamed";
        changed.m_state = "Invited";
        changed.m_clusterEnabled = false;
        changed.m_queueMaxSize = 64;
        store.updateNode(changed);

        const auto reread = store.node(nodeId);
        ASSERT_TRUE(reread.has_value()) << name;
        EXPECT_EQ(String("node-1-renamed"), reread->m_name) << name;
        EXPECT_EQ(String("Invited"), reread->m_state) << name;
        EXPECT_FALSE(reread->m_clusterEnabled) << name;
        ASSERT_TRUE(reread->m_queueMaxSize.has_value()) << name;
        EXPECT_EQ(64, *reread->m_queueMaxSize) << name << ": the edited limit was not written back";
        // A column left alone keeps what it had, which is what makes a partial update by an older
        // tool harmless.
        ASSERT_TRUE(reread->m_queueMaxBytes.has_value()) << name;
        EXPECT_EQ(1'048'576, *reread->m_queueMaxBytes) << name;

        // One row per node, one node per name: the second declaration of a name is refused by the
        // database, which is the only place that can decide it for every node at once.
        auto duplicate = sampleNode("7a2d3c8f-0b4e-4d66-9f32-1c8e6b0d5a21");
        duplicate.m_name = "node-1-renamed";
        EXPECT_THROW(store.insertNode(duplicate), Exception) << name << ": a duplicate name was accepted";
    }
}

// Reading a row and writing it back unchanged has to leave it unchanged - the interface edits a
// row it has just read, and a field that does not survive that round trip is a field that turns
// into garbage the first time somebody presses Save. Written because the logging section did
// exactly that.
TEST_F(XMQ_ClusterStoreTests, aRowSurvivesBeingReadAndWrittenBackUnchanged)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();

        const String nodeId = "2c7d1e44-5b6a-4f83-9d02-7a1b3c5e6f90";
        auto         inserted = sampleNode(nodeId);
        // Longer than the driver's first fetch buffer, which is what made a text column come back
        // truncated at 256 bytes with its full length reported - a certificate is read the same way.
        inserted.m_certificate = String(1200, '-') + "END CERTIFICATE-";
        store.insertNode(inserted);

        const auto read = store.node(nodeId);
        ASSERT_TRUE(read.has_value()) << name;
        ASSERT_TRUE(read->m_logging.has_value()) << name << ": the logging section did not come back";
        EXPECT_EQ(*inserted.m_logging, *read->m_logging) << name << ": the section changed on the way out";

        EXPECT_EQ(inserted.m_certificate, read->m_certificate) << name << ": the certificate was truncated";

        store.updateNode(*read);

        const auto again = store.node(nodeId);
        ASSERT_TRUE(again.has_value()) << name;
        ASSERT_TRUE(again->m_logging.has_value()) << name;
        EXPECT_EQ(*inserted.m_logging, *again->m_logging) << name << ": writing the row back changed it";
        EXPECT_EQ(inserted.m_certificate, again->m_certificate) << name << ": the certificate changed";
    }
}

TEST_F(XMQ_ClusterStoreTests, sqliteIsNotAClusterDatabase)
{
    const auto path = filesystem::temp_directory_path() / ("xmq_cluster_store_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);

    ClusterStore store(make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(path), 1));
    EXPECT_FALSE(store.isClusterDatabase()) << "a file on one machine cannot be a cluster's store";

    filesystem::remove(path);
}

TEST_F(XMQ_ClusterStoreTests, anInvitationReplacesTheOneBeforeIt)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();

        const String nodeId = "5e2f1a9d-7c4b-4e68-9d30-2a8c7f6b1d04";
        store.insertNode(sampleNode(nodeId));

        EXPECT_FALSE(store.invitation(nodeId).has_value()) << name << ": a new node has an invitation";

        ClusterStore::Invitation first;
        first.m_nodeId = nodeId;
        first.m_inviterHostPort = "localhost:8883";
        first.m_expiresAt = "2026-10-19T20:00:00Z";
        store.setInvitation(first);

        const auto read = store.invitation(nodeId);
        ASSERT_TRUE(read.has_value()) << name;
        EXPECT_EQ(String("localhost:8883"), read->m_inviterHostPort) << name;

        // The second invite is the administrator saying the first did not arrive: one per node,
        // the latest wins.
        ClusterStore::Invitation second = first;
        second.m_inviterHostPort = "localhost:8884";
        store.setInvitation(second);

        const auto reread = store.invitation(nodeId);
        ASSERT_TRUE(reread.has_value()) << name;
        EXPECT_EQ(String("localhost:8884"), reread->m_inviterHostPort) << name;

        store.clearInvitation(nodeId);
        EXPECT_FALSE(store.invitation(nodeId).has_value()) << name << ": the invitation did not go away";
    }
}

TEST_F(XMQ_ClusterStoreTests, removeNodeTakesItsInvitationWithIt)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();

        const String nodeId = "3c8b6a2e-9d17-4f53-8e24-1b7c5d9f0a36";
        store.insertNode(sampleNode(nodeId));

        ClusterStore::Invitation invitation;
        invitation.m_nodeId = nodeId;
        invitation.m_inviterHostPort = "localhost:8883";
        invitation.m_expiresAt = "2026-10-19T20:00:00Z";
        store.setInvitation(invitation);

        store.removeNode(nodeId);

        EXPECT_FALSE(store.node(nodeId).has_value()) << name << ": the node is still declared";
        EXPECT_FALSE(store.invitation(nodeId).has_value()) << name << ": the invitation outlived the node";
        EXPECT_EQ(0U, store.nodes().size()) << name << ": the node is still listed";
    }
}

TEST_F(XMQ_ClusterStoreTests, theStoredStateIsKeptApartFromTheComputedOne)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();

        const String nodeId = "8d4c1f3b-2e67-4a95-9f08-5c3d7b1e6a24";
        store.insertNode(sampleNode(nodeId));

        auto [state, error] = store.stateOf(nodeId);
        EXPECT_EQ(String("Not Clustered"), state) << name;
        EXPECT_TRUE(error.empty()) << name;

        // The state the interface writes: Invited while the invitation is out, Failed with the
        // reason once a join does not happen. Clustered and Offline never reach the table - the
        // screen computes those from Redis.
        auto row = store.node(nodeId);
        row->m_state = "Invited";
        store.updateNode(*row);

        tie(state, error) = store.stateOf(nodeId);
        EXPECT_EQ(String("Invited"), state) << name;

        row->m_state = "Failed";
        row->m_lastError = "the storage did not answer";
        store.updateNode(*row);

        tie(state, error) = store.stateOf(nodeId);
        EXPECT_EQ(String("Failed"), state) << name;
        EXPECT_EQ(String("the storage did not answer"), error) << name;

        store.removeNode(nodeId);
        tie(state, error) = store.stateOf(nodeId);
        EXPECT_TRUE(state.empty() && error.empty()) << name << ": a removed node has no stored state";
    }
}

TEST_F(XMQ_ClusterStoreTests, removingConfigurationKeepsATombstoneAndHidesItFromTheList)
{
    for (const auto& [name, uri]: m_databases)
    {
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        store.createSchema();
        const String id = "00000000-0000-4000-8000-000000000099";
        auto row = sampleNode(id);
        store.insertNode(row);
        store.removeConfiguration(id);
        const auto removed = store.node(id);
        ASSERT_TRUE(removed.has_value());
        EXPECT_EQ("Removed", removed->m_state);
        EXPECT_FALSE(removed->m_clusterEnabled);
        EXPECT_TRUE(store.nodes().empty());
        EXPECT_THROW(store.insertNode(row), Exception) << name;
    }
}

TEST_F(XMQ_ClusterStoreTests, theBridgeEraTableIsDroppedWhenTheSchemaIsCreated)
{
    for (const auto& [name, uri]: m_databases)
    {
        DatabaseConnectionPool pool(uri, 2);
        {
            const AutoDatabaseConnection connection(pool);
            Query create(connection.connection(),
                         "CREATE TABLE xmq_node (id int primary key, node_state int, node_name varchar(128),"
                         " settings varchar(4096), connected_nodes varchar(4096), updated timestamp)");
            create.exec();
        }

        // The old shape cannot be read as the new one; creating the schema replaces it, so a
        // database that saw the bridge era comes out with today's table and no leftovers.
        ClusterStore store(make_shared<DatabaseConnectionPool>(uri, 2));
        EXPECT_NO_THROW(store.createSchema()) << name;
        EXPECT_NO_THROW(store.insertNode(sampleNode("9b1e5d7a-4c28-4f76-8e03-6a2d0c9b4e57"))) << name;
        EXPECT_TRUE(store.node("9b1e5d7a-4c28-4f76-8e03-6a2d0c9b4e57").has_value()) << name;
    }
}
