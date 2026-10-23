#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/ControlServiceTests/ControlServiceTests.h"
#include "test/TestServers.h"
#include "server/Cluster/NodeIdentity.h"
#include "server/Settings/ClusterStore.h"
#include "server/Settings/UserStore.h"
#include "server/Settings/PasswordHash.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
struct SqlSchemaCleanup
{
    shared_ptr<DatabaseConnectionPool> pool;
    string schema;
    string previousOptions;
    bool hadOptions;
    ~SqlSchemaCleanup()
    {
        if (hadOptions) setenv("PGOPTIONS", previousOptions.c_str(), 1);
        else unsetenv("PGOPTIONS");
        try
        {
            const AutoDatabaseConnection connection(*pool);
            Query drop(connection.connection(), "DROP SCHEMA " + schema + " CASCADE");
            drop.exec();
        }
        catch (const Exception&) {}
    }
};

struct JoinCleanup
{
    shared_ptr<DatabaseConnectionPool> pool;
    vector<string> names;
    vector<string> ids;
    ~JoinCleanup()
    {
        for (const auto& name: names) XMQ_ClusterTests::stopNode(name);
        try
        {
            ClusterStore store(pool);
            for (const auto& id: ids) store.removeNode(id);
        }
        catch (const Exception&) {}
    }
};

String adminToken(ControlService& service)
{
    CLogin login;
    login.m_username = "admin";
    login.m_password = "admin";
    CLoginResponse response;
    service.Login(login, response, nullptr);
    EXPECT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString();
    return response.m_token.asString();
}
} // namespace

TEST_F(XMQ_ClusterTests, sharedDatabaseJoinNeedsOneConfirmationAndCanLeaveAgain)
{
    const auto configured = getenv("XMQ_TEST_JOIN_DATABASE_URI");
    const String uri = configured && *configured ? configured : "postgresql://gtest:test#123@dbhost_pg/xmq_test";
    if (uri.find("postgresql://") != 0)
    {
        GTEST_SKIP() << "This test isolates its accounts in a PostgreSQL schema";
    }
    const auto basePool = make_shared<DatabaseConnectionPool>(uri, 2);
    try
    {
        const AutoDatabaseConnection connection(*basePool);
        Query query(connection.connection(), "SELECT 1");
        query.open();
        query.close();
    }
    catch (const Exception&)
    {
        GTEST_SKIP() << "A dedicated shared SQL test database is required";
    }
    const string schema = "join_test_" + to_string(::getpid());
    const auto oldOptions = getenv("PGOPTIONS");
    SqlSchemaCleanup schemaCleanup {basePool, schema, oldOptions ? oldOptions : "", oldOptions != nullptr};
    {
        const AutoDatabaseConnection connection(*basePool);
        Query create(connection.connection(), "CREATE SCHEMA " + schema);
        create.exec();
    }
    setenv("PGOPTIONS", ("-c search_path=" + schema).c_str(), 1);
    const auto pool = make_shared<DatabaseConnectionPool>(uri, 2);
    UserStore users(pool);
    users.createSchema();
    const auto adminId = users.addUser({.m_username = "admin", .m_password = PasswordHash::hash("admin", 1000)});
    users.addUser({.m_username = "cluster", .m_password = PasswordHash::hash("cluster", 1000)});
    const auto administrators = users.addGroup({.m_name = String(string(UserStore::administratorsGroupName))});
    users.addUserToGroup(adminId, administrators);
    const string sourceName = "join-source-" + to_string(::getpid());
    const string targetName = "join-target-" + to_string(::getpid());
    JoinCleanup cleanup {pool, {sourceName, targetName}, {}};
    const auto source = createNode(sourceName, 1880, true);
    auto target = createNode(targetName, 1886, true);
    const auto sourceSettings = source->getSettings();
    const auto targetSettings = target->getSettings();
    const auto targetId = target->getCluster()->nodeId();
    cleanup.ids = {source->getCluster()->nodeId(), targetId};

    sourceSettings->setAccountDatabaseUri(uri);
    sourceSettings->loadConfiguration(sourceSettings->configurationPath());
    ASSERT_TRUE(sourceSettings->applyClusterEnable(TestServers::redisUri(), uri).empty());
    source->getCluster()->startup();
    trustNodeCertificate(target);
    // A known UUID and password do not authorise moving a server out of a different database.
    const auto differentDatabase = source->getCluster()->sendInvitation("localhost:8886", targetName, "", true, targetId);
    EXPECT_FALSE(differentDatabase.m_success);
    EXPECT_FALSE(targetSettings->m_cluster.m_enabled.asBool());

    targetSettings->setAccountDatabaseUri(uri);
    targetSettings->loadConfiguration(targetSettings->configurationPath());
    ClusterStore store(pool);
    ASSERT_TRUE(store.node(targetId).has_value());
    const auto wrongIdentity = source->getCluster()->sendInvitation("localhost:8886", targetName, "", true,
                                                                   cluster::NodeIdentity::generate());
    EXPECT_FALSE(wrongIdentity.m_success);
    EXPECT_FALSE(store.node(targetId)->m_clusterEnabled);

    TestServerController sourceController(source.get(), sourceSettings);
    ControlService sourceControl(&sourceController);
    HttpAuthentication authentication("bearer " + adminToken(sourceControl));
    CClusterControl request;
    request.m_action = "join";
    request.m_node.m_node_id = targetId;
    CClusterControlResponse response;
    sourceControl.ClusterControl(request, response, &authentication);
    ASSERT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString();
    EXPECT_TRUE(store.node(targetId)->m_clusterEnabled);
    EXPECT_EQ("Joining", store.node(targetId)->m_state);
    EXPECT_FALSE(target->getCluster()->invitations().current().has_value());

    // Simulate the service manager's restart, using the accepted configuration unchanged.
    stopNode(targetName);
    target.reset();
    targetSettings->loadConfiguration(targetSettings->configurationPath());
    auto joined = make_shared<Server>(targetSettings, logEngine());
    ASSERT_TRUE(TestCluster::waitFor([&]
    {
        return source->getCluster()->getConnectedNodeCount(targetName) > 0 &&
               joined->getCluster()->getConnectedNodeCount(sourceName) > 0;
    }));
    ASSERT_NE(nullptr, joined->getCluster()->coordinator());
    EXPECT_EQ(2U, joined->getCluster()->coordinator()->members().size());

    TestServerController targetController(joined.get(), targetSettings);
    ControlService targetControl(&targetController);
    HttpAuthentication targetAuthentication("bearer " + adminToken(targetControl));
    CClusterControl leave;
    leave.m_action = "disable";
    CClusterControlResponse left;
    targetControl.ClusterControl(leave, left, &targetAuthentication);
    ASSERT_TRUE(left.m_result.m_success.asBool()) << left.m_result.m_description.asString();
    EXPECT_EQ("Standalone", store.node(targetId)->m_state);
    EXPECT_FALSE(store.node(targetId)->m_clusterEnabled);
    EXPECT_EQ(1U, source->getCluster()->coordinator()->members().size());
    joined.reset();

    targetSettings->loadConfiguration(targetSettings->configurationPath());
    const auto standalone = make_shared<Server>(targetSettings, logEngine());
    EXPECT_EQ(nullptr, standalone->getCluster()->coordinator());
    EXPECT_TRUE(store.node(targetId).has_value());
}
