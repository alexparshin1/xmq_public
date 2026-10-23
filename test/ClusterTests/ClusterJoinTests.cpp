#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/ControlServiceTests/ControlServiceTests.h"
#include "test/TestServers.h"
#include "server/Cluster/NodeIdentity.h"
#include "server/Settings/ClusterStore.h"
#include "server/Settings/UserStore.h"
#include "server/Settings/PasswordHash.h"
#include "server/OSService.h"
#include <sptk5/net/TCPSocket.h>

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

struct ControllerCleanup
{
    ServerController& controller;
    ~ControllerCleanup() { controller.stopService(); }
};

String webRequest(const String& method, const String& body, const String& token = {})
{
    TCPSocket socket;
    socket.open(Host("localhost", 18886));
    const String request = "POST /" + method + " HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nConnection: close\r\n" +
        (token.empty() ? String() : "Authorization: bearer " + token + "\r\n") +
        "Content-Length: " + to_string(body.size()) + "\r\n\r\n" + body;
    socket.write(request);
    String response;
    uint8_t buffer[4096];
    while (socket.readyToRead(chrono::seconds(2)))
    {
        try
        {
            const auto bytes = socket.read(buffer, sizeof(buffer));
            if (bytes == 0) break;
            response.append(reinterpret_cast<const char*>(buffer), bytes);
        }
        catch (const Exception&) { break; }
    }
    return response;
}

String webClusterState(const String& token)
{
    return webRequest("ClusterControl", R"({"action":"get"})", token);
}

String webLogin()
{
    const auto response = webRequest("Login", R"({"username":"admin","password":"admin"})");
    const auto body = response.find("\r\n\r\n");
    if (body == String::npos) return {};
    xdoc::Document document;
    document.load(response.substr(body + 4));
    const auto token = document.root()->getString("token");
    EXPECT_FALSE(token.empty()) << response;
    return token;
}

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
    users.setClusterSecret("cluster");
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

    // Transfer the standalone node to a real controller that owns MQTT and a separate web listener.
    stopNode(targetName);
    target.reset();
    targetSettings->m_web_service.m_listener_port = 18886;
    targetSettings->m_web_service.m_encrypted = false;
    ServerController targetController(targetSettings);
    ControllerCleanup controllerCleanup {targetController};
    targetController.startService();
    ASSERT_TRUE(targetController.isServing());
    const auto targetToken = webLogin();
    ASSERT_FALSE(targetToken.empty());
    const auto restartBefore = OSService::restartRequested();
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));

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
    EXPECT_FALSE(targetController.getServer()->getCluster()->invitations().current().has_value());

    ASSERT_TRUE(TestCluster::waitFor([&]
    {
        OSService::processNodeActions(targetController);
        const auto joined = targetController.getServer();
        return joined && joined->getCluster()->coordinator() &&
               source->getCluster()->getConnectedNodeCount(targetName) > 0 &&
               joined->getCluster()->getConnectedNodeCount(sourceName) > 0;
    }, chrono::seconds(15)));
    EXPECT_EQ(2U, targetController.getServer()->getCluster()->coordinator()->members().size());
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));
    EXPECT_EQ(restartBefore, OSService::restartRequested());

    CClusterControl prematureRetire;
    prematureRetire.m_action = "retire";
    prematureRetire.m_node.m_node_id = targetId;
    CClusterControlResponse refusedRetire;
    sourceControl.ClusterControl(prematureRetire, refusedRetire, &authentication);
    EXPECT_FALSE(refusedRetire.m_result.m_success.asBool());
    EXPECT_TRUE(store.node(targetId)->m_clusterEnabled);

    CClusterControl leave;
    leave.m_action = "disable";
    leave.m_node.m_node_id = targetId;
    CClusterControlResponse left;
    sourceControl.ClusterControl(leave, left, &authentication);
    ASSERT_TRUE(left.m_result.m_success.asBool()) << left.m_result.m_description.asString();
    // The peer only acknowledges the request on its MQTT thread. Its controller drains safely.
    EXPECT_TRUE(store.node(targetId)->m_clusterEnabled);
    ASSERT_TRUE(TestCluster::waitFor([&]
    {
        OSService::processNodeActions(targetController);
        return targetController.getServer() && !targetSettings->m_cluster.m_enabled.asBool();
    }));
    EXPECT_TRUE(sourceSettings->m_cluster.m_enabled.asBool());
    EXPECT_EQ("Standalone", store.node(targetId)->m_state);
    EXPECT_FALSE(store.node(targetId)->m_clusterEnabled);
    EXPECT_EQ(1U, source->getCluster()->coordinator()->members().size());
    ASSERT_NE(nullptr, targetController.getServer());
    EXPECT_EQ(nullptr, targetController.getServer()->getCluster()->coordinator());
    EXPECT_TRUE(store.node(targetId).has_value());
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));
    EXPECT_EQ(restartBefore, OSService::restartRequested());

    // Leave through this node's own HTTP listener as well. The handler must only
    // acknowledge: the service loop owns stopping and replacing the Server.
    CClusterControlResponse rejoined;
    sourceControl.ClusterControl(request, rejoined, &authentication);
    ASSERT_TRUE(rejoined.m_result.m_success.asBool()) << rejoined.m_result.m_description.asString();
    targetController.isStopped(0ms);
    ASSERT_TRUE(targetSettings->m_cluster.m_enabled.asBool());
    const auto beforeLocalLeave = targetController.getServer();
    const auto localToken = webLogin();
    const auto localLeave = webRequest("ClusterControl",
        "{\"action\":\"disable\",\"node\":{\"node_id\":\"" + targetId +
        "\",\"name\":\"" + targetName + "\"}}", localToken);
    EXPECT_NE(String::npos, localLeave.find("200 OK")) << localLeave;
    xdoc::Document leaveResponse;
    leaveResponse.load(localLeave.substr(localLeave.find("\r\n\r\n") + 4));
    ASSERT_NE(nullptr, leaveResponse.root()->findFirst("result")) << localLeave;
    ASSERT_TRUE(leaveResponse.root()->findFirst("result")->getBoolean("success")) << localLeave;
    EXPECT_EQ(beforeLocalLeave, targetController.getServer());
    EXPECT_TRUE(targetSettings->m_cluster.m_enabled.asBool());
    EXPECT_TRUE(store.node(targetId)->m_clusterEnabled);
    targetController.isStopped(0ms);
    ASSERT_NE(nullptr, targetController.getServer());
    EXPECT_FALSE(targetSettings->m_cluster.m_enabled.asBool());
    EXPECT_FALSE(store.node(targetId)->m_clusterEnabled);
    EXPECT_EQ(nullptr, targetController.getServer()->getCluster()->coordinator());
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));

    // Simulate Redis becoming unavailable between admission checks and the MQTT start.
    ASSERT_TRUE(targetSettings->applyInvitationAccept("redis://127.0.0.1:1/2", uri).empty());
    OSService::queueNodeAction(targetId, uri, "reload");
    OSService::processNodeActions(targetController);
    ASSERT_NE(nullptr, targetController.getServer()) << "Previous standalone MQTT was not recovered";
    EXPECT_FALSE(targetSettings->m_cluster.m_enabled.asBool());
    EXPECT_EQ("Failed", store.node(targetId)->m_state);
    EXPECT_FALSE(store.node(targetId)->m_lastError.empty());
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));
    EXPECT_EQ(restartBefore, OSService::restartRequested());

    // A queued command must not operate after the node switches to another accounts database.
    OSService::queueNodeAction(targetId, "postgresql://another/database", "retire");
    OSService::processNodeActions(targetController);
    EXPECT_EQ("Failed", store.node(targetId)->m_state);
    EXPECT_TRUE(targetController.isServing());

    CClusterControl retire;
    retire.m_action = "retire";
    retire.m_node.m_node_id = targetId;
    CClusterControlResponse retired;
    sourceControl.ClusterControl(retire, retired, &authentication);
    ASSERT_TRUE(retired.m_result.m_success.asBool()) << retired.m_result.m_description.asString();
    OSService::processNodeActions(targetController);
    EXPECT_EQ("Retired", store.node(targetId)->m_state);
    EXPECT_FALSE(targetController.isServing());
    EXPECT_NE(String::npos, webClusterState(targetToken).find(targetId));
    EXPECT_TRUE(sourceSettings->m_cluster.m_enabled.asBool());
    EXPECT_TRUE(sourceController.isServing());
}
