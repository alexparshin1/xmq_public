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

#include "ControlServiceTests.h"

#include "server/Settings/UserStore.h"
#include "server/Settings/PasswordHash.h"
#include "test/TestServers.h"
#include "server/OSService.h"
#include <sptk5/net/TCPSocket.h>
#include "test/ClusterTests/TestCluster.h"
#include "common/RedisCredentials.h"

#include <cstdlib>
#include <filesystem>
#include "common/DirectoryNames.h"

using namespace std;
using namespace sptk;

namespace xmq {

void XMQ_ControlServiceTests::SetUp()
{
    XMQ_ServerTests::SetUp();
    m_controller = make_shared<TestServerController>(server().get(), server()->getSettings());
    m_controlService = make_shared<ControlService>(m_controller.get());
}

namespace {
void verifyResult(const CResult& result)
{
    if (result.m_success != true)
    {
        FAIL() << result.m_description.asString().c_str();
    }
}
} // namespace

std::string XMQ_ControlServiceTests::Login(const string& username, const string& password) const
{
    CLogin login;
    login.m_username = username;
    login.m_password = password;

    CLoginResponse output;
    m_controlService->Login(login, output, nullptr);

    if (output.m_result.m_success != true)
    {
        throw Exception(format("Login failed: {}", output.m_result.m_description.asString().c_str()));
    }
    return output.m_token.asString().c_str();
}

TEST_F(XMQ_ControlServiceTests, login)
{
    // As admin:
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());

    // As regular user:
    EXPECT_ANY_THROW({ token = Login("user", "secret"); });
}

TEST_F(XMQ_ControlServiceTests, settingsTestsConnectionsWithoutSavingAndMasksPasswords)
{
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    const auto settings = server()->getSettings();
    Buffer before;
    before.loadFromFile(settings->configurationPath());
    CSettingsControl request;
    request.m_action = "test";
    request.m_settings = settings->nodeSettings();
    CSettingsControlResponse tested;
    m_controlService->SettingsControl(request, tested, &authentication);
    ASSERT_TRUE(tested.m_result.m_success.asBool()) << tested.m_result.m_description.asString();
    EXPECT_FALSE(tested.m_pending.asBool());
    request.m_settings.m_database_uri = "postgresql://probe:Secret7@127.0.0.1:1/unavailable";
    CSettingsControlResponse refused;
    m_controlService->SettingsControl(request, refused, &authentication);
    EXPECT_FALSE(refused.m_result.m_success.asBool());
    Buffer after;
    after.loadFromFile(settings->configurationPath());
    EXPECT_EQ(String(before), String(after));
    const auto oldDatabase = settings->m_authentication.m_database_uri.asString();
    const auto oldRedis = settings->m_persistence.m_redis_uri.asString();
    settings->m_authentication.m_database_uri = "postgresql://xmq:db-secret@theater/xmq";
    settings->m_persistence.m_redis_uri = "redis://cache:redis-secret@theater:6379";
    request.m_action = "get";
    CSettingsControlResponse masked;
    m_controlService->SettingsControl(request, masked, &authentication);
    settings->m_authentication.m_database_uri = oldDatabase;
    settings->m_persistence.m_redis_uri = oldRedis;
    ASSERT_TRUE(masked.m_result.m_success.asBool());
    EXPECT_EQ("postgresql://xmq:*****@theater/xmq", masked.m_settings.m_database_uri.asString());
    EXPECT_EQ("redis://cache:*****@theater:6379", masked.m_settings.m_redis_uri.asString());
}

TEST_F(XMQ_ControlServiceTests, settingsApplyOnControllerThreadAndRecoverFromFailedPortChange)
{
    auto initial = server();
    const auto settings = initial->getSettings();
    stopServers();
    initial.reset();
    CWebService webService = settings->m_web_service;
    webService.m_listener_port = 18889;
    webService.m_encrypted = false;
    settings->m_persistence.m_clean_start = false;
    settings->serviceControl("set", webService);
    CLogging controllerLogging = settings->m_logging;
    controllerLogging.m_log_to = (settings->configurationPath().parent_path() / "controller.log").string();
    settings->loggingControl("set", controllerLogging);
    ServerController controller(settings);
    struct Cleanup { ServerController& controller; ~Cleanup() { controller.stopService(); } } cleanup {controller};
    controller.startService();
    ASSERT_TRUE(controller.isServing());
    ControlService service(&controller);
    CLogin login;
    login.m_username = "admin"; login.m_password = "admin";
    CLoginResponse loggedIn;
    service.Login(login, loggedIn, nullptr);
    ASSERT_TRUE(loggedIn.m_result.m_success.asBool());
    HttpAuthentication authentication("bearer " + loggedIn.m_token.asString());
    UserStore users(settings->accountsPool());
    const auto adminHash = users.findUser("admin")->m_password;
    client::MqttClient persistent(logEngine());
    ASSERT_EQ(ReasonCode::Success, persistent.connect(Host("localhost", 1880),
        ConnectCredentials("settings-persist-client", "user", "secret"), {.m_cleanSession = false}));
    persistent.disconnect();
    ASSERT_TRUE(TestCluster::waitFor([&] {
        const auto session = controller.getServer()->getClientSession("settings-persist-client");
        return session && !session->isConnected();
    }));
    CSettingsControl request;
    request.m_action = "set";
    request.m_settings = settings->nodeSettings();
    request.m_settings.m_node_name = "settings-renamed";
    request.m_settings.m_mqtt_port = 1885;
    request.m_settings.m_mqtt_ssl_port = 8885;
    CSettingsControlResponse queued;
    const auto oldPort = settings->nodeSettings().m_mqtt_port.asInteger();
    service.SettingsControl(request, queued, &authentication);
    ASSERT_TRUE(queued.m_result.m_success.asBool()) << queued.m_result.m_description.asString();
    EXPECT_TRUE(queued.m_pending.asBool());
    EXPECT_EQ(oldPort, settings->nodeSettings().m_mqtt_port.asInteger());
    OSService::processNodeActions(controller);
    EXPECT_FALSE(settings->nodeSettingsStatus().first);
    EXPECT_NE(String::npos, settings->nodeSettingsStatus().second.find("Settings applied"))
        << settings->nodeSettingsStatus().second;
    ASSERT_TRUE(controller.isServing());
    EXPECT_EQ(1885, settings->nodeSettings().m_mqtt_port.asInteger());
    EXPECT_EQ("settings-renamed", settings->nodeSettings().m_node_name.asString());
    EXPECT_NE(nullptr, controller.getServer()->getClientSession("settings-persist-client"));
    EXPECT_EQ(adminHash, users.findUser("admin")->m_password);
    request.m_settings = settings->nodeSettings();
    TCPSocket occupied;
    occupied.host(Host("0.0.0.0", 18890));
    occupied.listen(18890, false);
    request.m_settings.m_web_service_port = 18890;
    request.m_settings.m_mqtt_port = 1887;
    CSettingsControlResponse failing;
    service.SettingsControl(request, failing, &authentication);
    ASSERT_TRUE(failing.m_result.m_success.asBool()) << failing.m_result.m_description.asString();
    OSService::processNodeActions(controller);
    EXPECT_FALSE(settings->nodeSettingsStatus().first);
    EXPECT_NE(String::npos, settings->nodeSettingsStatus().second.find("Previous settings restored"));
    ASSERT_TRUE(controller.isServing());
    EXPECT_EQ(1885, settings->nodeSettings().m_mqtt_port.asInteger());
    EXPECT_EQ(18889, settings->nodeSettings().m_web_service_port.asInteger());
    EXPECT_EQ(adminHash, users.findUser("admin")->m_password);
}

TEST_F(XMQ_ControlServiceTests, initialSetupChecksRedisAuthentication)
{
    const auto configured = std::getenv("XMQ_TEST_REDIS_AUTH_URI");
    if (!configured || !*configured) GTEST_SKIP() << "An isolated authenticated Redis is required";
    const auto connection = redisConnectionUrl(string(configured));
    const auto& [host, port] = connection.hostAndPort();
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    CInitialSetupControl request;
    request.m_action = "check-redis";
    request.m_redis_host = host;
    request.m_redis_port = port;
    request.m_redis_username = connection.username();
    request.m_redis_password = connection.password();
    CInitialSetupControlResponse accepted;
    m_controlService->InitialSetupControl(request, accepted, &authentication);
    ASSERT_TRUE(accepted.m_result.m_success.asBool()) << accepted.m_result.m_description.asString();
    request.m_redis_username = ""; // password-only must authenticate as default
    CInitialSetupControlResponse passwordOnly;
    m_controlService->InitialSetupControl(request, passwordOnly, &authentication);
    EXPECT_TRUE(passwordOnly.m_result.m_success.asBool()) << passwordOnly.m_result.m_description.asString();
    request.m_redis_password = "Incorrect-Secret7!";
    CInitialSetupControlResponse refused;
    m_controlService->InitialSetupControl(request, refused, &authentication);
    EXPECT_FALSE(refused.m_result.m_success.asBool());
    request.m_redis_username = "missing-user";
    request.m_redis_password = connection.password();
    CInitialSetupControlResponse wrongUser;
    m_controlService->InitialSetupControl(request, wrongUser, &authentication);
    EXPECT_FALSE(wrongUser.m_result.m_success.asBool());
}

TEST_F(XMQ_ControlServiceTests, initialSetupChecksRedisWithoutApplyingConfiguration)
{
    const auto settings = server()->getSettings();
    Buffer before;
    before.loadFromFile(settings->configurationPath());
    const auto serving = m_controller->isServing();
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    const URL redis(TestServers::redisUri());
    const auto& [host, port] = redis.hostAndPort();
    CInitialSetupControl request;
    request.m_action = "check-redis";
    request.m_redis_host = host;
    request.m_redis_port = port;
    CInitialSetupControlResponse response;
    m_controlService->InitialSetupControl(request, response, &authentication);
    ASSERT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString();
    request.m_redis_host = "127.0.0.1";
    request.m_redis_port = 1;
    CInitialSetupControlResponse unavailable;
    m_controlService->InitialSetupControl(request, unavailable, &authentication);
    EXPECT_FALSE(unavailable.m_result.m_success.asBool());
    EXPECT_NE(String::npos, unavailable.m_result.m_description.asString().find("cannot be reached"));
    request.m_redis_port = 65536;
    CInitialSetupControlResponse invalid;
    m_controlService->InitialSetupControl(request, invalid, &authentication);
    EXPECT_FALSE(invalid.m_result.m_success.asBool());
    Buffer after;
    after.loadFromFile(settings->configurationPath());
    EXPECT_EQ(String(before), String(after));
    EXPECT_EQ(serving, m_controller->isServing());
}

TEST_F(XMQ_ControlServiceTests, initialSetupRefusesUnavailablePostgresOrMysqlBeforeApplying)
{
    const auto settings = server()->getSettings();
    Buffer before;
    before.loadFromFile(settings->configurationPath());
    const auto serving = m_controller->isServing();
    const auto listenerPort = settings->m_web_service.m_listener_port.asInteger();
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    for (const auto* type: {"postgresql", "mysql"})
    {
        SCOPED_TRACE(type);
        CInitialSetupControl request;
        request.m_action = "set";
        request.m_setup.m_admin_password = "Sunflower7!";
        request.m_setup.m_cluster_password = "Cluster-Secret1";
        request.m_setup.m_database_uri = String(type) + "://probe:Secret7@127.0.0.1:1/unavailable";
        CInitialSetupControlResponse response;
        m_controlService->InitialSetupControl(request, response, &authentication);
        EXPECT_FALSE(response.m_result.m_success.asBool());
        EXPECT_NE(String::npos, response.m_result.m_description.asString().find("127.0.0.1"))
            << response.m_result.m_description.asString();
        Buffer after;
        after.loadFromFile(settings->configurationPath());
        EXPECT_EQ(String(before), String(after));
        EXPECT_EQ(serving, m_controller->isServing());
        EXPECT_EQ(listenerPort, settings->m_web_service.m_listener_port.asInteger());
    }
}

TEST_F(XMQ_ControlServiceTests, initialSetupInitialisesAnEmptySharedUserDatabase)
{
    const auto* uri = std::getenv("XMQ_TEST_SETUP_DATABASE_URI");
    if (uri == nullptr || *uri == '\0')
    {
        GTEST_SKIP() << "Set XMQ_TEST_SETUP_DATABASE_URI to an empty PostgreSQL/MySQL test database";
    }

    const auto pool = make_shared<DatabaseConnectionPool>(uri, 2);
    {
        const AutoDatabaseConnection connection(*pool);
        const auto schema = pool->driverName().toLowerCase().starts_with("postgres")
                                ? "current_schema()" : "DATABASE()";
        Query tables(connection.connection(),
                     String("SELECT count(*) AS table_count FROM information_schema.tables ") +
                     "WHERE table_name = 'xmq_user' AND table_schema = " + schema);
        // The test database is dedicated to this test. UserStore must not be initialised here:
        // the setup preflight is the operation whose handling of a fresh database is under test.
        tables.open();
        ASSERT_EQ(0, tables["table_count"].asInteger());
        tables.close();
    }

    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    CInitialSetupControl request;
    request.m_action = "set";
    request.m_setup.m_admin_password = "Sunflower7!";
    request.m_setup.m_cluster_password = "Cluster-Secret1";
    request.m_setup.m_database_uri = uri;
    // Stop after the database preflight, before applying any configuration or certificate changes.
    request.m_setup.m_mqtt_port = 0;
    CInitialSetupControlResponse response;
    m_controlService->InitialSetupControl(request, response, &authentication);
    EXPECT_FALSE(response.m_result.m_success.asBool());
    EXPECT_NE(String::npos, response.m_result.m_description.asString().find("MQTT port"))
        << response.m_result.m_description.asString();

    UserStore users(pool);
    EXPECT_FALSE(users.findUser("admin").has_value());
    users.addUser({.m_username = "admin", .m_password = PasswordHash::hash("Marigold3?", 1000)});

    CInitialSetupControlResponse mismatch;
    m_controlService->InitialSetupControl(request, mismatch, &authentication);
    EXPECT_FALSE(mismatch.m_result.m_success.asBool());
    EXPECT_NE(String::npos, mismatch.m_result.m_description.asString().find("does not match"))
        << mismatch.m_result.m_description.asString();
    const auto admin = users.findUser("admin");
    ASSERT_TRUE(admin.has_value());
    EXPECT_TRUE(PasswordHash::verify("Marigold3?", admin->m_password));
}

TEST_F(XMQ_ControlServiceTests, adminWithoutAPasswordIsAdmittedByTheInterfaceOnly)
{
    const auto settings = server()->getSettings();
    auto&      users = settings->userManager();

    // A freshly installed server, made here by taking the password off the account the rest of
    // the suite signs in with. Put back at the end: the settings are shared by every test.
    //
    // The record is kept and handed back whole. What it holds is a verifier, which modifyUser()
    // stores as it is - so the password comes back exactly as it was, without this test having to
    // know it. Writing the password back as text would not work anyway: the suite's is 'admin',
    // and passwords chosen now have to be stronger than that.
    const auto original = users.findUser("admin");

    auto administrator = original;
    administrator.m_password = "";
    users.modifyUser(administrator);
    EXPECT_FALSE(settings->administratorPasswordSet());

    // A password that was never set matches nothing - including the empty one, which is what a
    // client that sent no password at all arrives with. This is the answer the MQTT listeners
    // get, and it is why the account cannot be used to connect to the broker.
    EXPECT_FALSE(users.authenticate("admin", ""));
    EXPECT_FALSE(users.authenticate("admin", "admin"));

    // The interface admits it regardless, and says what has to be done about it.
    CLogin login;
    login.m_username = "admin";
    login.m_password = "";
    CLoginResponse response;
    m_controlService->Login(login, response, nullptr);
    verifyResult(response.m_result);
    EXPECT_TRUE(response.m_setup_required.asBool());
    ASSERT_FALSE(response.m_token.asString().empty());

    // The token it issued has to work too: every other operation re-checks the credentials the
    // token carries, and would refuse them for the same reason authenticate() does.
    HttpAuthentication         authentication("bearer " + response.m_token.asString());
    CGetClientSessions         request;
    request.m_limit = 100;
    CGetClientSessionsResponse sessions;
    m_controlService->GetClientSessions(request, sessions, &authentication);
    verifyResult(sessions.m_result);

    // It is the administrator with no password that is admitted, not every account and not
    // every password.
    EXPECT_ANY_THROW({ (void) Login("admin", "wrong"); });
    EXPECT_ANY_THROW({ (void) Login("user", ""); });

    users.modifyUser(original);
    EXPECT_TRUE(settings->administratorPasswordSet());
    EXPECT_NO_THROW({ (void) Login("admin", "admin"); });
}

TEST_F(XMQ_ControlServiceTests, loggingControl)
{
    CLoggingControl         input;
    CLoggingControlResponse output;

    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    // Start from what is in force: a CLogging sent whole replaces every field, and the subjects
    // this test does not name would otherwise become empty - which the next Settings apply then
    // refuses, with a message about SubjectLogLevels.disconnect rather than about this test.
    input.m_action = "get";
    m_controlService->LoggingControl(input, output, &authentication);
    verifyResult(output.m_result);
    const auto originalLogging = output.m_logging;

    struct RestoreLogging
    {
        ControlService&     service;
        HttpAuthentication& authentication;
        CLogging            logging;
        ~RestoreLogging()
        {
            CLoggingControl request;
            request.m_action = "set";
            request.m_logging = logging;
            CLoggingControlResponse ignored;
            service.LoggingControl(request, ignored, &authentication);
        }
    } restoreLogging {*m_controlService, authentication, originalLogging};

    input.m_action = "set";
    input.m_logging = originalLogging;
    input.m_logging.m_min_log_level = "WARNING";
    input.m_logging.m_log_to = (DirectoryNames::tempDirectory() / "test.log").string();
    input.m_logging.m_log_level.m_connect = "INFO";
    input.m_logging.m_log_level.m_publish = "DEBUG";
    m_controlService->LoggingControl(input, output, &authentication);
    verifyResult(output.m_result);

    input.m_action = "get";
    m_controlService->LoggingControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_STREQ("WARNING", output.m_logging.m_min_log_level.getString());
    const auto expectedLogPath = (DirectoryNames::tempDirectory() / "test.log").string();
    EXPECT_STREQ(expectedLogPath.c_str(), output.m_logging.m_log_to.getString());
    EXPECT_STREQ("INFO", output.m_logging.m_log_level.m_connect.getString());
    EXPECT_STREQ("DEBUG", output.m_logging.m_log_level.m_publish.getString());
}

TEST_F(XMQ_ControlServiceTests, userControl)
{
    CUserControl         input;
    CUserControlResponse output;

    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    // Count existing users:
    input.m_action = "list";
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_LE(2U, output.m_list.size());
    auto originalUserCount = output.m_list.size();

    CUser user;
    user.m_id = 10;
    user.m_username = "user1";
    user.m_password = "Password-1!";
    user.m_is_enabled = true;

    // Add new user:
    input.m_action = "add";
    input.m_user = user;
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    // SQL assigns the persistent ID; edits use the ID the list will return.
    user.m_id = server()->getSettings()->userManager().findUser("user1").m_id;
    // The password is checked directly rather than by signing in: signing in also requires the
    // account to be an administrator, which is a membership now, and this test is about the
    // account's password rather than about what the account is allowed to do.
    EXPECT_TRUE(server()->getSettings()->userManager().authenticate(user.m_username, "Password-1!"));

    // Count users:
    input.m_action = "list";
    input.m_user.clear();
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(originalUserCount + 1, output.m_list.size());

    // Modify the new user's password:
    input.m_action = "modify";
    input.m_user = user;
    input.m_user.m_password = "Password-2!";
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_FALSE(server()->getSettings()->userManager().authenticate(user.m_username, "Password-1!"));
    EXPECT_TRUE(server()->getSettings()->userManager().authenticate(user.m_username, "Password-2!"));

    // Remove the new user:
    input.m_action = "remove";
    input.m_user = user;
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_ANY_THROW(Login(user.m_username, "Password-2!"));

    // Count users:
    input.m_action = "list";
    input.m_user.clear();
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(originalUserCount, output.m_list.size());
}

/**
 * @brief Changing anonymous access through UserControl does not require a user record.
 */
TEST_F(XMQ_ControlServiceTests, userControlChangesAnonymousAccessWithoutAUserRecord)
{
    const auto token = Login("admin", "admin");
    HttpAuthentication authentication("bearer " + token);
    const auto& users = server()->getSettings()->userManager();
    const auto original = users.isAllowAnonymous();

    CUserControl input;
    CUserControlResponse output;
    input.m_action = "modify";
    input.m_allow_anonymous = !original;
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(!original, users.isAllowAnonymous());

    input.m_allow_anonymous = original;
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(original, users.isAllowAnonymous());
}

TEST_F(XMQ_ControlServiceTests, listenerControl)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    // List existing listeners
    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "list";
        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);
        EXPECT_GE(output.m_list.size(), 1U);
    }

    // Add a new listener, then list, then remove.
    // Deliberately below the ephemeral port range floor (net.ipv4.ip_local_port_range,
    // 10000 on the tuned hosts): a listener port inside that range is occasionally taken
    // as the source port of one of the suite's own client connections, and the bind then
    // fails with "address already in use" in whichever test loses the race.
    constexpr uint16_t testPort = 9884;

    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "add";
        input.m_listener.m_port = testPort;
        input.m_listener.m_threads = 1;
        input.m_listener.m_protocol = "mqtt";

        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);
    }

    int listenerId = 0;

    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "list";
        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);

        const bool found = std::ranges::any_of(
            output.m_list,
            [&listenerId](const auto& l)
            {
                if (static_cast<uint16_t>(l.m_port.asInteger()) == testPort)
                {
                    listenerId = l.m_id.asInteger();
                    return true;
                }
                return false;
            });

        EXPECT_TRUE(found);
    }

    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "remove";
        input.m_listener.m_id = listenerId;
        input.m_listener.m_port = testPort;
        input.m_listener.m_protocol = "mqtt";
        input.m_listener.m_threads = 1;

        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);
    }
}

TEST_F(XMQ_ControlServiceTests, persistenceControl)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    CPersistenceControl         input;
    CPersistenceControlResponse output;

    // Baseline
    input.m_action = "get";
    m_controlService->PersistenceControl(input, output, &authentication);
    verifyResult(output.m_result);
    const auto original = output.m_persistence;

    // This is the fixture's own Settings object, shared by every test in the suite. Put the
    // persistence settings back on every exit path, or the next test finds Redis at
    // localhost:6379 - a stopped server on that port, and a failure that depends on the seed.
    struct RestorePersistence
    {
        ControlService&     service;
        HttpAuthentication& authentication;
        CPersistence        persistence;
        ~RestorePersistence()
        {
            CPersistenceControl request;
            request.m_action = "set";
            request.m_persistence = persistence;
            CPersistenceControlResponse ignored;
            service.PersistenceControl(request, ignored, &authentication);
        }
    } restorePersistence {*m_controlService, authentication, original};

    // Set (reuse whatever was returned but change one field if possible)
    input.m_action = "set";
    input.m_persistence = original;
    input.m_persistence.m_redis_uri = "redis://localhost:6379";
    m_controlService->PersistenceControl(input, output, &authentication);
    verifyResult(output.m_result);

    // Get again - should succeed and be consistent
    input.m_action = "get";
    input.m_persistence.clear();
    m_controlService->PersistenceControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_STREQ("redis://localhost:6379", output.m_persistence.m_redis_uri.getString());
}

TEST_F(XMQ_ControlServiceTests, limitsControl)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    CLimitsControl         input;
    CLimitsControlResponse output;

    input.m_action = "get";
    m_controlService->LimitsControl(input, output, &authentication);
    verifyResult(output.m_result);

    // Round-trip: set what we got, then get again
    input.m_action = "set";
    input.m_server_limits = output.m_server_limits;
    input.m_queue_limits = output.m_queue_limits;

    CLimitsControlResponse setOutput;
    m_controlService->LimitsControl(input, setOutput, &authentication);
    verifyResult(setOutput.m_result);

    input.m_action = "get";
    input.m_server_limits.clear();
    input.m_queue_limits.clear();
    m_controlService->LimitsControl(input, output, &authentication);
    verifyResult(output.m_result);
}

TEST_F(XMQ_ControlServiceTests, serviceControl)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    CServiceControl         input;
    CServiceControlResponse output;

    input.m_action = "get";
    m_controlService->ServiceControl(input, output, &authentication);
    verifyResult(output.m_result);
}

TEST_F(XMQ_ControlServiceTests, getSubscriptions)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    CGetSubscriptions         input;
    CGetSubscriptionsResponse output;
    m_controlService->GetSubscriptions(input, output, &authentication);
    verifyResult(output.m_result);
    // Content depends on the server state; at least ensure the call succeeds.
}

TEST_F(XMQ_ControlServiceTests, getClientSessions)
{
    string token;
    EXPECT_NO_THROW({ token = Login("admin", "admin"); });
    EXPECT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    const auto manager = server()->getClientSessionManager();
    const auto addSession = [&](const string& id, const bool clean) {
        auto parameters = make_shared<ConnectMessageParameters>();
        parameters->setClientId(id);
        parameters->m_cleanSession = clean;
        auto session = ClientSession::factory(server().get(), parameters);
        manager->add(session);
        return session;
    };
    const auto first = addSession("sessions-test-one", false);
    const auto second = addSession("sessions-test-two", true);
    const auto outside = addSession("other-sessions-test-one", false);

    CGetClientSessions input;
    input.m_prefix = "sessions-test-";
    input.m_limit = 1;
    CGetClientSessionsResponse output;
    m_controlService->GetClientSessions(input, output, &authentication);
    verifyResult(output.m_result);
    ASSERT_EQ(output.m_client_sessions.size(), 1U);
    EXPECT_EQ(output.m_client_sessions[0].m_client_id.asString(), "sessions-test-one");
    EXPECT_TRUE(output.m_has_more.asBool());
    EXPECT_EQ(output.m_client_sessions[0].m_queued_messages.asInteger(), 0);
    EXPECT_EQ(output.m_client_sessions[0].m_subscription_count.asInteger(), 0);
    EXPECT_FALSE(output.m_client_sessions[0].m_online.asBool());

    input.m_limit = 2;
    output = CGetClientSessionsResponse();
    m_controlService->GetClientSessions(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(output.m_client_sessions.size(), 2U);
    EXPECT_FALSE(output.m_has_more.asBool());
    EXPECT_EQ(output.m_client_sessions[0].m_client_id.asString(), "sessions-test-one");
    EXPECT_EQ(output.m_client_sessions[1].m_client_id.asString(), "sessions-test-two");
    for (const auto& session: output.m_client_sessions)
    {
        EXPECT_EQ(session.m_persistent.asBool(), session.m_client_id.asString() == "sessions-test-one");
    }

    input.m_limit = 501;
    output = CGetClientSessionsResponse();
    m_controlService->GetClientSessions(input, output, &authentication);
    EXPECT_FALSE(output.m_result.m_success.asBool());

    input.m_limit = 10;
    input.m_prefix = "sessions-test-(";
    output = CGetClientSessionsResponse();
    m_controlService->GetClientSessions(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_TRUE(output.m_client_sessions.empty());
    EXPECT_FALSE(output.m_has_more.asBool());

    manager->remove(first);
    manager->remove(second);
    manager->remove(outside);
}


namespace {

/**
 * @brief Puts this suite's accounts into a database of their own, so groups can be exercised.
 *
 * The suite builds its configuration from a buffer rather than a file, and a configuration that
 * came from nowhere has nowhere to put a default database - so these tests have to say where.
 * The accounts move across first: adopting an empty database would take them away, and the login
 * every one of these tests starts with would go with them.
 */
void giveTheAccountsADatabase(const std::shared_ptr<Settings>& settings)
{
    const auto file = filesystem::temp_directory_path() / format("xmq_control_groups_{}.db", getpid());
    filesystem::remove(file);

    auto store = make_shared<UserStore>(
        make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2));
    store->createSchema();

    for (const auto& user: settings->userManager().getUsers(".*", false))
    {
        store->addUser({.m_username = user.m_username.asString(),
                        .m_password = user.m_password.asString(),
                        .m_enabled = user.m_is_enabled.asBool()});
    }

    settings->userManager().useStore(store);

    // Being an administrator is a membership now, so the account this suite signs in as has to be
    // in the group - it was a field on the account before, and the accounts were copied above.
    const auto [defaultGroup, administratorsGroup] = store->createStandardGroups();
    if (const auto administrator = store->findUser("admin"))
    {
        store->addUserToGroup(administrator->m_id, defaultGroup);
        store->addUserToGroup(administrator->m_id, administratorsGroup);
    }
}

} // namespace

// Asked before anyone has signed in, by a page deciding whether to offer a sign-in form. A server
// that has never been set up has an administrator with no password and no other way in, so the
// form would have to be explained rather than used.
TEST_F(XMQ_ControlServiceTests, setupStateAnswersWithoutASignIn)
{
    CSetupState         input;
    CSetupStateResponse output;

    // No authentication of any kind, which is the whole point of this operation.
    m_controlService->SetupState(input, output, nullptr);
    verifyResult(output.m_result);

    // This suite's administrator has a password, so there is nothing to set up.
    EXPECT_FALSE(output.m_setup_required.asBool())
        << "a server whose administrator has a password asked to be set up";

    // And with the password taken away it says so - the state a fresh installation is in.
    //
    // Put back by a destructor rather than at the end of the test: every other test in this suite
    // begins by signing in as this account, and leaving it without a password would fail all of
    // them for a reason that has nothing to do with what they measure.
    struct RestoreThePassword
    {
        UserManager& m_users;
        CUser        m_administrator {m_users.findUser("admin")};

        explicit RestoreThePassword(UserManager& users)
            : m_users(users)
        {}

        ~RestoreThePassword()
        {
            m_users.modifyUser(m_administrator);
        }
    };

    RestoreThePassword restore {server()->getSettings()->userManager()};

    CUser withoutAPassword(restore.m_administrator);
    withoutAPassword.m_password = "";
    server()->getSettings()->userManager().modifyUser(withoutAPassword);

    m_controlService->SetupState(input, output, nullptr);
    verifyResult(output.m_result);
    EXPECT_TRUE(output.m_setup_required.asBool())
        << "an administrator with no password did not ask to be set up";
}

// Groups and membership, as the users screen will use them: the screen shows an account with its
// groups and sends both back together.
TEST_F(XMQ_ControlServiceTests, groupsAndMembershipThroughTheInterface)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    const auto makeGroup = [&](const String& name)
    {
        CUserGroupControl         input;
        CUserGroupControlResponse output;
        input.m_action = "add";
        input.m_group.m_name = name;
        m_controlService->UserGroupControl(input, output, &authentication);
        verifyResult(output.m_result);
    };

    const auto listGroups = [&]
    {
        CUserGroupControl         input;
        CUserGroupControlResponse output;
        input.m_action = "list";
        m_controlService->UserGroupControl(input, output, &authentication);
        verifyResult(output.m_result);

        Strings names;
        for (const auto& group: output.m_list)
        {
            names.push_back(String(group.m_name.asString()));
        }
        return names;
    };

    const auto groupsOfUser = [&](const String& username)
    {
        CUserControl         input;
        CUserControlResponse output;
        input.m_action = "list";
        m_controlService->UserControl(input, output, &authentication);
        verifyResult(output.m_result);

        Strings names;
        for (const auto& listed: output.m_list)
        {
            if (String(listed.m_username.asString()) == username)
            {
                for (const auto& group: listed.m_groups)
                {
                    names.push_back(String(group.asString()));
                }
            }
        }
        return names;
    };

    makeGroup("operators");
    makeGroup("auditors");

    const auto groups = listGroups();
    EXPECT_NE(groups.end(), ranges::find(groups, "operators"));
    EXPECT_NE(groups.end(), ranges::find(groups, "auditors"));

    // An account arrives with its whole membership, and that is what it ends up with.
    CUserControl         addUser;
    CUserControlResponse addResponse;
    addUser.m_action = "add";
    addUser.m_user.m_username = "grouped-user";
    addUser.m_user.m_password = "Some-Password1";
    WSString groupName;
    groupName = "operators";
    addUser.m_user.m_groups.push_back(groupName);
    m_controlService->UserControl(addUser, addResponse, &authentication);
    verifyResult(addResponse.m_result);

    EXPECT_EQ(Strings {"operators"}, groupsOfUser("grouped-user"));

    // Saving it again with both groups adds the second and keeps the first: the screen sends the
    // whole membership every time and does not work out what changed.
    CUserControl         modifyUser;
    CUserControlResponse modifyResponse;
    modifyUser.m_action = "modify";
    modifyUser.m_user.m_username = "grouped-user";
    modifyUser.m_user.m_password = "*****";
    for (const auto* name: {"operators", "auditors"})
    {
        WSString entry;
        entry = name;
        modifyUser.m_user.m_groups.push_back(entry);
    }
    m_controlService->UserControl(modifyUser, modifyResponse, &authentication);
    verifyResult(modifyResponse.m_result);

    const auto both = groupsOfUser("grouped-user");
    EXPECT_EQ(2U, both.size()) << both.join(", ");

    // Removing a group takes the membership of it with it, and leaves the account alone.
    CUserGroupControl         removeGroup;
    CUserGroupControlResponse removeResponse;
    removeGroup.m_action = "remove";
    removeGroup.m_group.m_name = "auditors";
    m_controlService->UserGroupControl(removeGroup, removeResponse, &authentication);
    verifyResult(removeResponse.m_result);

    EXPECT_EQ(Strings {"operators"}, groupsOfUser("grouped-user"))
        << "removing a group did not take its memberships with it";
}

// A group that is not there is named as an error rather than created, or a name typed wrongly on
// the screen would quietly become a group of its own.
TEST_F(XMQ_ControlServiceTests, anUnknownGroupIsRefusedRatherThanCreated)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    CUserControl         input;
    CUserControlResponse output;
    input.m_action = "add";
    input.m_user.m_username = "hopeful-user";
    input.m_user.m_password = "Some-Password1";
    WSString mistyped;
    mistyped = "operatrs";
    input.m_user.m_groups.push_back(mistyped);
    m_controlService->UserControl(input, output, &authentication);

    EXPECT_FALSE(output.m_result.m_success.asBool())
        << "an account was saved into a group nobody created";
    EXPECT_NE(String::npos, String(output.m_result.m_description.asString()).find("operatrs"))
        << "the answer does not say which group was not found";
}


// A broker nobody can administer cannot be undone from the interface - the only way back is the
// setup path, and only while the administrator has no password. So the last enabled administrator
// is held in place, by whichever of the four routes leads there.
TEST_F(XMQ_ControlServiceTests, theLastAdministratorCannotBeRemovedDisabledOrMovedOut)
{
    giveTheAccountsADatabase(server()->getSettings());
    auto& users = server()->getSettings()->userManager();

    ASSERT_TRUE(users.isAdministrator("admin")) << "the suite's administrator is not one";

    CUser administrator = users.findUser("admin");

    // Deleted.
    EXPECT_THROW(users.removeUser(administrator), Exception)
        << "the only administrator was deleted";

    // Disabled.
    CUser disabled(administrator);
    disabled.m_is_enabled = false;
    EXPECT_THROW(users.modifyUser(disabled), Exception)
        << "the only administrator was disabled";

    // Taken out of the group.
    EXPECT_THROW(users.setGroupsOf("admin", Strings {"Default"}), Exception)
        << "the only administrator was moved out of Administrators";

    // And the group itself, which empties it in one step.
    EXPECT_THROW(users.removeGroup("Administrators"), Exception)
        << "the group holding the only administrator was removed";

    EXPECT_TRUE(users.isAdministrator("admin")) << "the account did not survive the attempts";
}

// The rule is about the last one standing, not about a particular name: with somebody else able to
// administer, every account can be removed, disabled or moved - including the one called admin.
TEST_F(XMQ_ControlServiceTests, anAdministratorCanBeRemovedWhileAnotherRemains)
{
    giveTheAccountsADatabase(server()->getSettings());
    auto& users = server()->getSettings()->userManager();

    CUser second;
    second.m_username = "second-administrator";
    second.m_password = "Another-Pass1";
    second.m_is_enabled = true;
    users.addUser(second);
    users.setGroupsOf("second-administrator", Strings {"Default", "Administrators"});
    ASSERT_TRUE(users.isAdministrator("second-administrator"));

    // Now the stock account is no longer the only one, so it may go.
    const CUser administrator = users.findUser("admin");
    EXPECT_NO_THROW(users.setGroupsOf("admin", Strings {"Default"}))
        << "an administrator was held in place while another one existed";
    EXPECT_FALSE(users.isAdministrator("admin"));

    // Put back, since every other test in this suite signs in as it.
    users.setGroupsOf("admin", Strings {"Default", "Administrators"});
    users.setGroupsOf("second-administrator", Strings {"Default"});
    users.removeUser(users.findUser("second-administrator"));
}

} // namespace xmq
