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

#include "server/ServerController.h"
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
uint16_t freePrometheusPort()
{
    TCPSocket probe;
    probe.host(Host("127.0.0.1", 0));
    probe.listen(0, false);

    sockaddr_in address {};
    socklen_t   length = sizeof(address);
    if (::getsockname(probe.fd(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        probe.close();
        throw runtime_error("getsockname failed");
    }
    const auto port = ntohs(address.sin_port);
    probe.close();
    return port;
}

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

TEST_F(XMQ_ControlServiceTests, startServiceRefusesListenerHeldByAnotherBroker)
{
    const auto running = server();
    ASSERT_NE(nullptr, running);

    // Another broker, differing only in its node name, and pointing at the listeners the running
    // one already holds. It used to bind them too: the listener sockets are SO_REUSEPORT, so the
    // kernel accepted the second bind and split the clients between the two processes.
    auto settings = make_shared<Settings>();
    settings->loadConfiguration(running->getSettings()->configurationPath());
    settings->m_cluster.m_this_node.m_node_name = "second";
    settings->m_connections = running->getSettings()->m_connections;
    settings->m_web_service = running->getSettings()->m_web_service;
    // The controller builds a log engine before it tries to bind, and the loaded configuration
    // names the installed log path - which a test has no business writing to, and on a machine
    // where the broker is installed is a file owned by its service account. What this test is
    // about is the listener.
    settings->m_logging.m_log_to = (DirectoryNames::tempDirectory() / "xmq_second_broker.log").string();

    ServerController second(settings);
    try
    {
        second.startService();
        FAIL() << "A second broker started on the first broker's listener ports.";
    }
    catch (const Exception& exception)
    {
        const string message = exception.message().c_str();
        EXPECT_NE(string::npos, message.find("already in use")) << message;
        EXPECT_NE(string::npos, message.find(to_string(TestTcpPortNumber))) << message;
    }
}

TEST_F(XMQ_ControlServiceTests, prometheusSettingsAreReadAndAppliedThroughTheInterface)
{
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    const auto settings = server()->getSettings();
    const auto saved = settings->m_prometheus;

    CPrometheusControl get;
    get.m_action = "get";
    CPrometheusControlResponse current;
    m_controlService->PrometheusControl(get, current, &authentication);
    ASSERT_TRUE(current.m_result.m_success.asBool()) << current.m_result.m_description.asString();
    ASSERT_FALSE(current.m_settings.m_enabled.asBool());
    EXPECT_EQ(9184, current.m_settings.m_port.asInteger());
    EXPECT_FALSE(current.m_listening.asBool());

    CPrometheusControl set;
    set.m_action = "set";
    set.m_settings.m_enabled = true;
    set.m_settings.m_bind_ip = "127.0.0.1";
    set.m_settings.m_port = 29185;
    set.m_settings.m_username = "metrics";
    set.m_settings.m_password = "Secret1!";
    CPrometheusControlResponse applied;
    m_controlService->PrometheusControl(set, applied, &authentication);
    ASSERT_TRUE(applied.m_result.m_success.asBool()) << applied.m_result.m_description.asString();
    EXPECT_TRUE(applied.m_settings.m_enabled.asBool());
    EXPECT_EQ(29185, applied.m_settings.m_port.asInteger());
    EXPECT_TRUE(applied.m_listening.asBool());
    EXPECT_STREQ("metrics", applied.m_settings.m_username.asString().c_str());
    // The verifier is never sent back; a set password reads as a mask.
    EXPECT_STREQ("*****", applied.m_settings.m_password.asString().c_str());
    // Written, not only held: the next start has to come up with the same endpoint.
    EXPECT_TRUE(settings->prometheusSettings().m_enabled.asBool());
    EXPECT_EQ(29185, settings->prometheusSettings().m_port.asInteger());
    EXPECT_STREQ("metrics", settings->prometheusSettings().m_username.asString().c_str());
    EXPECT_TRUE(PasswordHash::isHashed(settings->m_prometheus.m_password.asString().c_str()));

    // Back to what this fixture started with; the server is shared by the tests that follow.
    settings->m_prometheus = saved;
}

TEST_F(XMQ_ControlServiceTests, prometheusEndpointStartsAndStopsWithoutRestartingTheBroker)
{
    const auto settings = server()->getSettings();
    const auto saved = settings->m_prometheus;
    const auto port = freePrometheusPort();

    CPrometheusSettings values;
    values.m_enabled = true;
    values.m_bind_ip = "127.0.0.1";
    values.m_port = port;
    settings->setPrometheusSettings(values);

    // The controller's constructor builds the log engine from these settings, and the running
    // server's log path is the installed one. Point it at the temp directory while the controller
    // is being built and put it back: the endpoint is what this test is about, not the log file.
    const auto savedLogTo = settings->m_logging.m_log_to;
    settings->m_logging.m_log_to = (DirectoryNames::tempDirectory() / "xmq_prometheus_controller.log").string();
    ServerController controller(settings);
    settings->m_logging.m_log_to = savedLogTo;
    string reason;
    ASSERT_TRUE(controller.applyPrometheusSettings(reason)) << reason;
    ASSERT_TRUE(controller.prometheusListening());

    TCPSocket client;
    client.open(Host("127.0.0.1", port), Socket::OpenMode::CONNECT, true, chrono::seconds(2));
    client.write(String("GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    string reply;
    array<uint8_t, 4096> buffer {};
    while (client.readyToRead(chrono::seconds(2)))
    {
        const auto received = client.read(buffer.data(), buffer.size());
        if (received == 0) break;
        reply.append(reinterpret_cast<const char*>(buffer.data()), received);
    }
    EXPECT_NE(string::npos, reply.find("HTTP/1.1 200 OK")) << reply;
    // The controller owns no MQTT server here, so the endpoint says so rather than disappearing.
    EXPECT_NE(string::npos, reply.find("xmq_up 0")) << reply;

    values.m_enabled = false;
    settings->setPrometheusSettings(values);
    ASSERT_TRUE(controller.applyPrometheusSettings(reason)) << reason;
    EXPECT_FALSE(controller.prometheusListening());

    // The raw section, not prometheusSettings(): the latter masks the verifier, and restoring a
    // mask after the hash was cleared would be a username without a password.
    settings->m_prometheus = saved;
}

TEST_F(XMQ_ControlServiceTests, prometheusRejectsAPortAnotherListenerUses)
{
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));
    const auto settings = server()->getSettings();
    const auto mqttPort = settings->m_connections.m_listener.front().m_port.asInteger();

    CPrometheusControl set;
    set.m_action = "set";
    set.m_settings.m_enabled = true;
    set.m_settings.m_bind_ip = "127.0.0.1";
    set.m_settings.m_port = mqttPort;
    CPrometheusControlResponse refused;
    m_controlService->PrometheusControl(set, refused, &authentication);
    EXPECT_FALSE(refused.m_result.m_success.asBool());
    EXPECT_NE(string::npos, string(refused.m_result.m_description.asString().c_str()).find("already used"))
        << refused.m_result.m_description.asString().c_str();

    set.m_settings.m_port = 80;
    CPrometheusControlResponse tooLow;
    m_controlService->PrometheusControl(set, tooLow, &authentication);
    EXPECT_FALSE(tooLow.m_result.m_success.asBool());
    EXPECT_NE(string::npos, string(tooLow.m_result.m_description.asString().c_str()).find("1025"))
        << tooLow.m_result.m_description.asString().c_str();

    set.m_settings.m_port = 29186;
    set.m_settings.m_username = "metrics";
    set.m_settings.m_password = "";
    CPrometheusControlResponse noPassword;
    m_controlService->PrometheusControl(set, noPassword, &authentication);
    EXPECT_FALSE(noPassword.m_result.m_success.asBool());
    EXPECT_NE(string::npos, string(noPassword.m_result.m_description.asString().c_str()).find("requires a password"))
        << noPassword.m_result.m_description.asString().c_str();

    set.m_settings.m_password = "short";
    CPrometheusControlResponse weakPassword;
    m_controlService->PrometheusControl(set, weakPassword, &authentication);
    EXPECT_FALSE(weakPassword.m_result.m_success.asBool());
    EXPECT_NE(string::npos, string(weakPassword.m_result.m_description.asString().c_str()).find("8 characters"))
        << weakPassword.m_result.m_description.asString().c_str();
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

// The dashboard asks before a start or a stop whether this node's stored state should go, and sends
// the answer with the request. What matters here is that the answer arrives at the controller and
// that a request without it says nothing - other callers must keep the configured behaviour.
TEST_F(XMQ_ControlServiceTests, serverControlCarriesTheCleanStateAnswer)
{
    string token;
    ASSERT_NO_THROW(token = Login("admin", "admin"));
    ASSERT_FALSE(token.empty());
    HttpAuthentication authentication("bearer " + token);

    const auto call = [this, &authentication](const String& action, const std::optional<bool>& cleanState)
    {
        CServerControl         request;
        CServerControlResponse response;
        request.m_action = action;
        if (cleanState)
        {
            request.m_clean_state = *cleanState;
        }
        m_controlService->ServerControl(request, response, &authentication);
        verifyResult(response.m_result);
    };

    call("start", true);
    ASSERT_TRUE(m_controller->cleanStateOnStart().has_value());
    EXPECT_TRUE(*m_controller->cleanStateOnStart());

    call("stop", false);
    ASSERT_TRUE(m_controller->cleanStateOnStop().has_value());
    EXPECT_FALSE(*m_controller->cleanStateOnStop());

    // Absent: the controller is told nothing, and a start or a stop keeps its configured meaning.
    m_controller->forgetCleanState();
    call("start", std::nullopt);
    call("stop", std::nullopt);
    EXPECT_FALSE(m_controller->cleanStateOnStart().has_value());
    EXPECT_FALSE(m_controller->cleanStateOnStop().has_value());
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

    // The port is served by the running broker, without a restart: the control adds and removes
    // listeners on the server it is talking to, and a listener that is only written to the
    // configuration would leave the interface reporting something the broker is not doing.
    const auto acceptsConnections = [](const uint16_t port)
    {
        try
        {
            TCPSocket socket;
            socket.open(Host("127.0.0.1", port), Socket::OpenMode::CONNECT, true, chrono::milliseconds(2000));
            return true;
        }
        catch (const Exception&)
        {
            return false;
        }
    };
    EXPECT_TRUE(acceptsConnections(testPort))
        << "the listener was added to the configuration but nothing serves the port";

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

    // An allow list wider than one address, written through the interface and listed back: the
    // listener is the record of it, and the button that edits it reads what the server holds.
    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "modify";
        input.m_listener.m_id = listenerId;
        input.m_listener.m_port = testPort;
        input.m_listener.m_name = "Allow listed";
        input.m_listener.m_protocol = "mqtt";
        input.m_listener.m_threads = 1;
        input.m_listener.m_whitelist = "127.0.0.1/32, 10.1.1.0/24,10.1.2.123";

        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);

        input.m_action = "list";
        m_controlService->ListenerControl(input, output, &authentication);
        verifyResult(output.m_result);

        const bool found = std::ranges::any_of(
            output.m_list,
            [&listenerId](const auto& l)
            {
                return l.m_id.asInteger() == listenerId;
            });
        EXPECT_TRUE(found);
        const auto listed = std::ranges::find_if(
            output.m_list,
            [&listenerId](const auto& l)
            {
                return l.m_id.asInteger() == listenerId;
            });
        ASSERT_NE(listed, output.m_list.end());
        EXPECT_EQ("127.0.0.1/32, 10.1.1.0/24,10.1.2.123", listed->m_whitelist.asString())
            << "the allow list did not come back from the listener it was written to";
    }

    // Refused before anything is changed: an allow list that does not parse would otherwise admit
    // nobody, and its author would not learn that until the ports went quiet.
    {
        CListenerControl         input;
        CListenerControlResponse output;

        input.m_action = "modify";
        input.m_listener.m_id = listenerId;
        input.m_listener.m_port = testPort;
        input.m_listener.m_name = "Allow listed";
        input.m_listener.m_protocol = "mqtt";
        input.m_listener.m_threads = 1;
        input.m_listener.m_whitelist = "10.1.1.999";

        m_controlService->ListenerControl(input, output, &authentication);
        EXPECT_FALSE(output.m_result.m_success) << "an impossible allow list was accepted";
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

    for (int attempt = 0; attempt < 20 && acceptsConnections(testPort); ++attempt)
    {
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    EXPECT_FALSE(acceptsConnections(testPort)) << "the removed listener still serves its port";
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

TEST_F(XMQ_ControlServiceTests, permissionsControlListsAddsTestsAndRemoves)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    CPermissionsControl         input;
    CPermissionsControlResponse output;

    // A group of its own per run: the accounts database outlives a failed run, and a test that
    // counts its own rules must not inherit somebody else's.
    const String suffix = to_string(::getpid());
    const String group = "user:test-permissions-" + suffix;
    const String username = "test-permissions-" + suffix;

    const auto add = [&](const String& action, const bool deny, const String& filter)
    {
        input.m_action = "add";
        input.m_rule.m_group = group;
        input.m_rule.m_action = action;
        input.m_rule.m_deny = deny;
        input.m_rule.m_filter = filter;
        m_controlService->PermissionsControl(input, output, &authentication);
        verifyResult(output.m_result);
    };

    // The deny comes first: order is the policy now, so an allow covering the secret ahead of it
    // would win.
    add("publish", true, "sensors/secret/#");
    add("publish", false, "sensors/#");

    // The list is canonical and reading it back does not duplicate anything.
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);
    size_t ownRules = 0;
    for (const auto& rule: output.m_rules)
    {
        ownRules += rule.m_group.asString() == group ? 1 : 0;
    }
    EXPECT_EQ(2U, ownRules);

    const auto test = [&](const String& topic)
    {
        input.m_action = "test";
        input.m_username = username;
        input.m_topic = topic;
        input.m_acl_action = "publish";
        m_controlService->PermissionsControl(input, output, &authentication);
        verifyResult(output.m_result);
        return String(output.m_decision.asString());
    };

    EXPECT_EQ("allow", test("sensors/temp"));
    EXPECT_EQ("deny", test("sensors/secret/key"));
    // The rules know this client, so a topic none of them covers is denied: Mosquitto's default.
    EXPECT_EQ("deny", test("somewhere/else"));

    // Remove both, so the shared accounts database is left as it was found.
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    for (const auto& rule: output.m_rules)
    {
        if (rule.m_group.asString() != group)
        {
            continue;
        }
        CPermissionsControl         remove;
        CPermissionsControlResponse removed;
        remove.m_action = "remove";
        remove.m_rule_id = rule.m_id.asInteger();
        m_controlService->PermissionsControl(remove, removed, &authentication);
        verifyResult(removed.m_result);
    }
}


TEST_F(XMQ_ControlServiceTests, permissionsImportPlansAppliesAndRefusesConflicts)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    const String suffix = to_string(::getpid());
    const String group = "user:import-" + suffix;
    const String username = "import-" + suffix;
    const auto   acl = "user " + string(username.c_str()) + "\ntopic readwrite sensors/#\n";
    const auto   denied = "user " + string(username.c_str()) + "\ntopic deny sensors/secret/#\n";

    CPermissionsControl         input;
    CPermissionsControlResponse output;

    const auto run = [&](const String& action, const String& content, const String& resolution = "")
    {
        input.m_action = action;
        input.m_format = "acl";
        input.m_content = content;
        input.m_resolution = resolution;
        m_controlService->PermissionsControl(input, output, &authentication);
        return output.m_result.m_success.asBool();
    };

    ASSERT_TRUE(run("import-plan", acl.c_str()));
    EXPECT_EQ(2, output.m_new_count.asInteger()) << "readwrite is a publish and a subscribe rule";
    EXPECT_FALSE(output.m_plan.asString().empty());

    ASSERT_TRUE(run("import-apply", acl.c_str()));
    ASSERT_TRUE(run("import-plan", acl.c_str()));
    EXPECT_EQ(0, output.m_new_count.asInteger());
    EXPECT_EQ(2, output.m_unchanged_count.asInteger()) << "a second import must not duplicate";

    // A deny on a topic the existing allow covers is a conflict. Applying without a decision fails.
    ASSERT_TRUE(run("import-plan", denied.c_str()));
    EXPECT_GE(output.m_conflict_count.asInteger(), 1);
    EXPECT_FALSE(run("import-apply", denied.c_str())) << "a conflict was applied without a decision";

    // Choosing the imported rule replaces the conflicting allow.
    ASSERT_TRUE(run("import-apply", denied.c_str(), "imported"));
    ASSERT_TRUE(run("import-plan", denied.c_str()));
    EXPECT_EQ(0, output.m_conflict_count.asInteger()) << "the conflicting allow is gone";

    // Leave the shared accounts database as it was found.
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    for (const auto& rule: output.m_rules)
    {
        if (rule.m_group.asString() != group)
        {
            continue;
        }
        CPermissionsControl         remove;
        CPermissionsControlResponse removed;
        remove.m_action = "remove";
        remove.m_rule_id = rule.m_id.asInteger();
        m_controlService->PermissionsControl(remove, removed, &authentication);
    }
}


TEST_F(XMQ_ControlServiceTests, permissionsRejectInvalidTopicFilters)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    CPermissionsControl         input;
    CPermissionsControlResponse output;
    const String group = "user:filter-" + to_string(::getpid());

    const auto add = [&](const String& filter)
    {
        input.m_action = "add";
        input.m_rule.m_group = group;
        input.m_rule.m_action = "subscribe";
        input.m_rule.m_deny = false;
        input.m_rule.m_filter = filter;
        output.m_result.m_success = false;
        m_controlService->PermissionsControl(input, output, &authentication);
        return output.m_result.m_success.asBool();
    };

    EXPECT_FALSE(add("//s/#")) << "an empty level is refused as a typo";
    EXPECT_FALSE(add("s//temp")) << "an empty level is refused in the middle too";
    EXPECT_FALSE(add("s/")) << "a trailing empty level is refused";
    EXPECT_FALSE(add("s/##")) << "# is a whole level and the last one";
    EXPECT_FALSE(add("s/+x")) << "+ is a whole level";
    EXPECT_TRUE(add("s/+/temp")) << "+ as a whole level is valid";
    EXPECT_TRUE(add("s/#")) << "# as the last whole level is valid";

    // Leave the shared accounts database as it was found.
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    for (const auto& rule: output.m_rules)
    {
        if (rule.m_group.asString() != group)
        {
            continue;
        }
        CPermissionsControl         remove;
        CPermissionsControlResponse removed;
        remove.m_action = "remove";
        remove.m_rule_id = rule.m_id.asInteger();
        m_controlService->PermissionsControl(remove, removed, &authentication);
    }
}


TEST_F(XMQ_ControlServiceTests, permissionsPatternRulesUseTheClientId)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    CPermissionsControl         input;
    CPermissionsControlResponse output;

    input.m_action = "add";
    input.m_rule.m_group = "*";
    input.m_rule.m_action = "subscribe";
    input.m_rule.m_deny = false;
    input.m_rule.m_filter = "devices/%c/#";
    input.m_rule.m_pattern = true;
    input.m_rule.m_source = "mosquitto";
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);

    const auto test = [&](const String& clientId, const String& topic)
    {
        input.m_action = "test";
        input.m_username = "";
        input.m_client_id = clientId;
        input.m_topic = topic;
        input.m_acl_action = "subscribe";
        m_controlService->PermissionsControl(input, output, &authentication);
        verifyResult(output.m_result);
        return String(output.m_decision.asString());
    };

    EXPECT_EQ("allow", test("device-7", "devices/device-7/temp"));
    EXPECT_EQ("deny", test("device-8", "devices/device-7/temp"));
    EXPECT_EQ("deny", test("+", "devices/+/temp")) << "a wildcard client id cannot widen a pattern";

    // Leave the shared accounts database as it was found.
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    for (const auto& rule: output.m_rules)
    {
        if (!rule.m_pattern.asBool())
        {
            continue;
        }
        CPermissionsControl         remove;
        CPermissionsControlResponse removed;
        remove.m_action = "remove";
        remove.m_rule_id = rule.m_id.asInteger();
        m_controlService->PermissionsControl(remove, removed, &authentication);
    }
}


TEST_F(XMQ_ControlServiceTests, userImportPlansAndAppliesMosquittoPasswords)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    const String suffix = to_string(::getpid());
    const String hashedUser = "mosq-hashed-" + suffix;
    const String plainUser = "mosq-plain-" + suffix;
    const String oldUser = "mosq-old-" + suffix;

    const string content =
        string(hashedUser.c_str()) +
        ":$7$1000$h0tqVxBwkB9rKAXukTtffzdbBQtNy1q5FBTDwSW4hucfjpqunBbxW10NVnRk7Cfh0lQndnOv2+k4wJavgz1JNw=="
        "$02ujkUXlKkJGFzlQHNjUgXwG3XRB1mr3vs8NX5teGCJGbN4hdgSpHNHuj47j8r5SHXsO7GeHpmkpPNhLraVVcQ==\n" +
        string(plainUser.c_str()) + ":Secret12!\n" +
        string(oldUser.c_str()) +
        ":$6$vZY4TS+/HBxHw38S$vvjVFECzb8dyuu/mruD2QKTfdFn0WmKxbc+1TsdB0L8EdHk3v9JRmfjHd56+VaTnUcSZOZ/hzkdvWCtxlX7AUQ==\n";

    CUserControl         input;
    CUserControlResponse output;
    input.m_action = "import-plan";
    input.m_content = content.c_str();
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    const int checkable = PasswordHash::supportsSha512Crypt() ? 1 : 0;
    EXPECT_EQ(2 + checkable, output.m_new_count.asInteger());
    EXPECT_EQ(1 - checkable, output.m_unsupported_count.asInteger());
    EXPECT_EQ(String::npos, output.m_plan.asString().find("$7$")) << "the plan leaked a verifier";
    EXPECT_EQ(String::npos, output.m_plan.asString().find("Secret12!")) << "the plan leaked a password";

    input.m_action = "import-apply";
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(2 + checkable, output.m_new_count.asInteger());

    auto& users = server()->getSettings()->userManager();
    EXPECT_TRUE(PasswordHash::verify("password", users.findUser(hashedUser.c_str()).m_password.asString()));
    EXPECT_TRUE(PasswordHash::verify("Secret12!", users.findUser(plainUser.c_str()).m_password.asString()));

    // The first successful login replaces Mosquitto's verifier with this broker's own.
    EXPECT_TRUE(users.authenticate(hashedUser.c_str(), "password"));
    EXPECT_TRUE(users.findUser(hashedUser.c_str()).m_password.asString().starts_with("pbkdf2-sha256$"))
        << "the Mosquitto verifier was not rehashed at login";
    EXPECT_TRUE(PasswordHash::verify("password", users.findUser(hashedUser.c_str()).m_password.asString()));
    if (PasswordHash::supportsSha512Crypt())
    {
        EXPECT_FALSE(users.findUser(oldUser.c_str()).m_password.asString().empty());
    }
    else
    {
        EXPECT_THROW(users.findUser(oldUser.c_str()), Exception)
            << "an unsupported verifier must not become an account nobody can use";
    }

    // A second import recognises both accounts and adds no duplicate.
    input.m_action = "import-plan";
    m_controlService->UserControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(0, output.m_new_count.asInteger());
    EXPECT_EQ(2 + checkable, output.m_existing_count.asInteger());

    // Leave the shared accounts database as it was found.
    for (const auto& name: {hashedUser, plainUser, oldUser})
    {
        try
        {
            users.removeUser(users.findUser(name.c_str()));
        }
        catch (const Exception&)
        {
        }
    }
}


TEST_F(XMQ_ControlServiceTests, permissionsPolicyAndTheImportTurnOnDefaultDeny)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    CPermissionsControl         input;
    CPermissionsControlResponse output;

    const auto policy = [&](const bool deny)
    {
        input.m_action = "set-policy";
        input.m_default_deny = deny;
        m_controlService->PermissionsControl(input, output, &authentication);
        verifyResult(output.m_result);
        return output.m_default_deny.asBool();
    };

    EXPECT_TRUE(policy(true));
    EXPECT_TRUE(output.m_default_deny.asBool());

    input.m_action = "test";
    input.m_username = "nobody-" + to_string(::getpid());
    input.m_topic = "some/topic";
    input.m_acl_action = "publish";
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ("deny", output.m_decision.asString())
        << "default deny covers a client the rules do not know";

    EXPECT_FALSE(policy(false));

    // An empty ACL changes nothing: opening or locking the broker on no evidence is worse than
    // leaving the current answer alone.
    input.m_action = "import-apply";
    input.m_format = "acl";
    input.m_content = "# nothing here\n";
    input.m_resolution = "reject";
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_FALSE(output.m_default_deny.asBool());

    // A file with a rule is a broker under ACL control.
    const String group = "user:policy-" + to_string(::getpid());
    const auto   acl = "user " + string(group.c_str()) + "\ntopic read sensors/#\n";
    input.m_content = acl.c_str();
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_TRUE(output.m_default_deny.asBool());

    // Leave the shared accounts database as it was found.
    EXPECT_FALSE(policy(false));
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    for (const auto& rule: output.m_rules)
    {
        if (rule.m_group.asString() != group)
        {
            continue;
        }
        CPermissionsControl         remove;
        CPermissionsControlResponse removed;
        remove.m_action = "remove";
        remove.m_rule_id = rule.m_id.asInteger();
        m_controlService->PermissionsControl(remove, removed, &authentication);
    }
}


TEST_F(XMQ_ControlServiceTests, mosquittoImportAppliesAccountsAndRulesInOneTransaction)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    const String suffix = to_string(::getpid());
    const String hashedUser = "atomic-hashed-" + suffix;
    const String plainUser = "atomic-plain-" + suffix;
    const String group = "user:atomic-rules-" + suffix;

    const string passwordContent =
        string(hashedUser.c_str()) +
        ":$7$1000$h0tqVxBwkB9rKAXukTtffzdbBQtNy1q5FBTDwSW4hucfjpqunBbxW10NVnRk7Cfh0lQndnOv2+k4wJavgz1JNw=="
        "$02ujkUXlKkJGFzlQHNjUgXwG3XRB1mr3vs8NX5teGCJGbN4hdgSpHNHuj47j8r5SHXsO7GeHpmkpPNhLraVVcQ==\n" +
        string(plainUser.c_str()) + ":Secret12!\n";
    const string aclContent = "user " + string(group.c_str()) + "\ntopic read sensors/#\n";

    CMosquittoImportControl         input;
    CMosquittoImportControlResponse output;
    input.m_action = "plan";
    input.m_password_content = passwordContent.c_str();
    input.m_acl_format = "acl";
    input.m_acl_content = aclContent.c_str();
    m_controlService->MosquittoImportControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(2, output.m_new_accounts.asInteger());
    EXPECT_EQ(1, output.m_new_rules.asInteger());
    EXPECT_EQ(0, output.m_conflicts.asInteger());
    EXPECT_FALSE(output.m_default_deny.asBool());

    input.m_action = "apply";
    input.m_resolution = "reject";
    m_controlService->MosquittoImportControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_EQ(2, output.m_new_accounts.asInteger());
    EXPECT_EQ(1, output.m_new_rules.asInteger());
    EXPECT_TRUE(output.m_default_deny.asBool());

    auto& users = server()->getSettings()->userManager();
    EXPECT_TRUE(PasswordHash::verify("password", users.findUser(hashedUser.c_str()).m_password.asString()));
    EXPECT_TRUE(PasswordHash::verify("Secret12!", users.findUser(plainUser.c_str()).m_password.asString()));
    EXPECT_EQ(String::npos, output.m_password_plan.asString().find("$7$"));
    EXPECT_EQ(String::npos, output.m_password_plan.asString().find("Secret12!"));

    // A conflict stops the whole apply: the account proposed beside it must not appear either.
    const String conflictUser = "atomic-conflict-" + suffix;
    const string conflictingPassword = string(conflictUser.c_str()) + ":Secret12!\n";
    const string conflictingAcl =
        "user atomic-conflict\ntopic read atomic/#\ntopic deny atomic/#\n";
    input.m_action = "apply";
    input.m_password_content = conflictingPassword.c_str();
    input.m_acl_content = conflictingAcl.c_str();
    m_controlService->MosquittoImportControl(input, output, &authentication);
    EXPECT_FALSE(output.m_result.m_success.asBool());
    EXPECT_THROW(users.findUser(conflictUser.c_str()), Exception)
        << "the account was written although the import was refused";

    // Leave the shared accounts database as it was found.
    for (const auto& name: {hashedUser, plainUser})
    {
        try
        {
            users.removeUser(users.findUser(name.c_str()));
        }
        catch (const Exception&)
        {
        }
    }
    const auto stored = server()->getSettings()->userManager().store();
    stored->setDefaultDeny(false);
    for (const auto& rule: stored->aclRules())
    {
        if (rule.m_groupName == group)
        {
            stored->removeAclRule(rule.m_id);
        }
    }
}


TEST_F(XMQ_ControlServiceTests, permissionsReplacePolicyKeepsTheOrderItIsGiven)
{
    giveTheAccountsADatabase(server()->getSettings());

    string token;
    ASSERT_NO_THROW({ token = Login("admin", "admin"); });
    HttpAuthentication authentication("bearer " + token);

    const String subject = "user:policy-order-" + to_string(::getpid());
    CPermissionsControl         input;
    CPermissionsControlResponse output;

    const auto replace = [&](const vector<pair<bool, String>>& entries)
    {
        input.m_action = "replace-policy";
        input.m_subject = subject;
        input.m_policy_rules.clear();
        for (const auto& [deny, filter]: entries)
        {
            CAclRule rule;
            rule.m_group = subject;
            rule.m_action = "subscribe";
            rule.m_deny = deny;
            rule.m_filter = filter;
            input.m_policy_rules.push_back(rule);
        }
        m_controlService->PermissionsControl(input, output, &authentication);
        return output.m_result.m_success.asBool();
    };

    const auto test = [&](const String& topic)
    {
        input.m_action = "test";
        input.m_username = "policy-order-" + to_string(::getpid());
        input.m_topic = topic;
        input.m_acl_action = "subscribe";
        m_controlService->PermissionsControl(input, output, &authentication);
        verifyResult(output.m_result);
        return String(output.m_decision.asString());
    };

    ASSERT_TRUE(replace({{false, "sensors/#"}, {true, "sensors/secret/#"}}));
    // The allow is first, so it covers the secret too: that is what first-match means.
    EXPECT_EQ("allow", test("sensors/secret/key"));

    ASSERT_TRUE(replace({{true, "sensors/secret/#"}, {false, "sensors/#"}}));
    EXPECT_EQ("deny", test("sensors/secret/key"));
    EXPECT_EQ("allow", test("sensors/temp"));

    // The list is the policy: emptying it removes the subject's rules.
    ASSERT_TRUE(replace({}));
    input.m_action = "list";
    m_controlService->PermissionsControl(input, output, &authentication);
    verifyResult(output.m_result);
    EXPECT_FALSE(ranges::any_of(output.m_rules, [&](const auto& rule)
                               { return rule.m_group.asString() == subject; }));

    // A repeated rule in one policy is refused rather than stored twice.
    EXPECT_FALSE(replace({{false, "sensors/#"}, {false, "sensors/#"}}));
}

} // namespace xmq
