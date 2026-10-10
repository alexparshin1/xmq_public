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

#include "server/Settings/PasswordHash.h"
#include "server/Settings/Settings.h"

#include "common/DirectoryNames.h"
#include "common/HostName.h"
#include "server/SelfSignedCertificate.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

static const String g_testConfig = R"({
        "connections": {
            "ssl_keys": {
                "cafile": "${ProgramCerts}/ca.crt",
                "keyfile": "${ProgramCerts}/server.key",
                "certfile": "${ProgramCerts}/server.crt",
                "verify_depth": 0,
            },
            "listener": [
                {
                    "id": 1,
                    "name": "Not Encrypted",
                    "protocol": "MQTT",
                    "bind_ip": "0.0.0.0",
                    "port": 1880,
                    "threads": 16,
                    "enable": true
                },
                {
                    "id": 2,
                    "name": "Encrypted",
                    "protocol": "MQTT+SSL",
                    "bind_ip": "0.0.0.0",
                    "port": 8880,
                    "threads": 1,
                    "enable": true
                }
            ]
        },
        "server_limits": {
            "send_threads": 3,
            "receive_threads": 3,
            "max_topic_alias": 128
        },
        "queue_limits": {
            "max_size": 16384,
            "max_inflight_messages": 128
        },
        "authentication": {
            "allow_anonymous": false,
            "users": [
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
                }
            ]
        },
        "persistence": {
            "redis_uri": "redis://localhost:6379",
            "clean_start": true,
            "max_threads": 4,
            "max_redis_connections": 32
        },
        "logging": {
            "log_to": "xmq_server.log",
            "min_log_level": "DEBUG",
            "log_level_available": ["DEBUG","INFO","NOTICE","WARNING","ERROR","PANIC"],
            "log_level": {
                "ack": "INFO",
                "connect": "DEBUG",
                "disconnect": "DEBUG",
                "publish": "DEBUG",
                "subscribe": "DEBUG",
                "unsubscribe": "DEBUG",
                "server_connections": "DEBUG",
                "server_events": "DEBUG",
                "cluster_connections": "DEBUG",
                "cluster_events": "DEBUG"
            },
        },
        "web_service": {
            "listener_port": 18880
        },
        "bridges": [
            {
                "id": 1,
                "node_name": "EMQX",
                "enabled": false,
                "host_port": "localhost:1883",
                "username": "user",
                "password": "secret",
                "client_id": "xmq_bridge",
                "clean_session": false,
                "topics": ["#"],
                "mode": "out"
            },
            {
                "id": 2,
                "node_name": "Mosquitto",
                "enabled": true,
                "host_port": "localhost:1882",
                "username": "user",
                "password": "secret",
                "client_id": "xmq_bridge",
                "clean_session": false,
                "topics": ["#"],
                "mode": "inout"
            },
            {
                "id": 3,
                "node_name": "xmq-1880",
                "enabled": true,
                "host_port": "localhost:1880",
                "username": "cluster",
                "password": "cluster",
                "client_id": "xmq_bridge",
                "clean_session": false,
                "topics": ["topic/1"],
                "mode": "cluster"
            },
            {
                "id": 4,
                "node_name": "xmq-1886",
                "enabled": true,
                "host_port": "localhost:1886",
                "username": "cluster",
                "password": "cluster",
                "client_id": "xmq_bridge",
                "clean_session": false,
                "topics": ["topic/2"],
                "mode": "cluster"
            }
        ],
        "cluster": {
            "this_node": {
                "node_name": "xmq-1880",
                "host_port": "localhost:1880",
            }
        }
    })";

TEST(XMQ_Settings, load)
{
    const Buffer config(g_testConfig);
    Settings     settings;
    settings.loadConfiguration(config);

    EXPECT_EQ(settings.m_connections.m_listener[0].m_port.asInteger(), 1880);
    EXPECT_STREQ(settings.m_connections.m_listener[0].m_bind_ip.asString().c_str(), "0.0.0.0");
    EXPECT_EQ(settings.m_connections.m_listener[0].m_threads.asInteger(), 16);

    EXPECT_EQ(settings.m_queue_limits.m_max_size.asInteger(), 16384);
    EXPECT_EQ(settings.m_queue_limits.m_max_inflight_messages.asInteger(), 128);

    EXPECT_TRUE(settings.userManager().authenticate("user", "secret"));
}

TEST(XMQ_Settings, subjectLogLevels)
{
    const Buffer config(g_testConfig);
    Settings     settings;
    settings.loadConfiguration(config);

    EXPECT_FALSE(settings.logSubjectEnabled(LogSubject::Ack, LogPriority::Debug));
    EXPECT_TRUE(settings.logSubjectEnabled(LogSubject::Publish, LogPriority::Debug));
    EXPECT_TRUE(settings.logSubjectEnabled(LogSubject::ClusterEvents, LogPriority::Debug));

    settings.setLogSubjectsPriority({LogSubject::ClusterEvents, LogSubject::ClusterConnections}, LogPriority::Info);
    EXPECT_FALSE(settings.logSubjectEnabled(LogSubject::ClusterEvents, LogPriority::Debug));
}

namespace {

/**
 * @brief Write the test configuration out as a template, and return where it landed.
 *
 * The initial setup reads a template from disk, so these tests need one; the installed template
 * is not used, because it is what the machine is configured with rather than something the tests
 * may rely on.
 */
/**
 * @brief A certificates directory that goes away with the test.
 *
 * Applying a setup issues this node's certificate, and the directory it would write it into on a
 * developer's machine belongs to that machine's own installation.
 */
class TemporaryCertificateDirectory
{
public:
    TemporaryCertificateDirectory()
        : m_directory(filesystem::temp_directory_path() /
                      ("xmq_settings_tests_" + to_string(::getpid())))
        // Put back rather than cleared: the run as a whole already points certificates at a
        // directory of its own, and clearing would aim whatever comes next at the machine's.
        , m_previous(DirectoryNames::certsDirectory())
    {
        error_code errorCode;
        filesystem::remove_all(m_directory, errorCode);
        DirectoryNames::setCertsDirectory(m_directory);
    }

    ~TemporaryCertificateDirectory()
    {
        DirectoryNames::setCertsDirectory(m_previous);
        error_code errorCode;
        filesystem::remove_all(m_directory, errorCode);
    }

    TemporaryCertificateDirectory(const TemporaryCertificateDirectory&) = delete;
    TemporaryCertificateDirectory& operator=(const TemporaryCertificateDirectory&) = delete;

    [[nodiscard]] const filesystem::path& path() const { return m_directory; }

private:
    filesystem::path m_directory;
    filesystem::path m_previous;
};

filesystem::path writeConfigurationTemplate()
{
    const auto templateFile = DirectoryNames::tempDirectory() / "xmq_settings_test.conf.template";
    const Buffer templateText(g_testConfig);
    templateText.saveToFile(templateFile);
    return templateFile;
}

/**
 * @brief The port a protocol is served on, or 0 when no listener serves it.
 */
int listenerPort(const Settings& settings, const String& protocol)
{
    for (const auto& listener: settings.m_connections.m_listener)
    {
        if (listener.m_protocol.asString() == protocol)
        {
            return listener.m_port.asInteger();
        }
    }
    return 0;
}

/**
 * @brief An empty directory of its own for a test that writes configuration files.
 */
filesystem::path emptyTestDirectory(const String& name)
{
    const auto directory = DirectoryNames::tempDirectory() / name.c_str();
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);
    return directory;
}

CInitialSetup testInitialSetup()
{
    CInitialSetup setup;
    setup.m_admin_password = "New-Secret1";
    setup.m_node_name = "xmq-1884";
    setup.m_mqtt_port = 1884;
    setup.m_mqtt_ssl_port = 8884;
    setup.m_web_service_port = 18884;
    setup.m_redis_host = "redis-host";
    setup.m_redis_port = 6380;
    return setup;
}

} // namespace

TEST(XMQ_Settings, initialSetupDefaults)
{
    const auto templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    const auto defaults = settings.initialSetup(templateFile);

    EXPECT_STREQ("xmq-1880", defaults.m_node_name.asString().c_str());
    EXPECT_EQ(1880, defaults.m_mqtt_port.asInteger());
    EXPECT_EQ(8880, defaults.m_mqtt_ssl_port.asInteger());
    EXPECT_EQ(18880, defaults.m_web_service_port.asInteger());
    // No address, because this template does not turn persistence on: it has a redis_uri but no
    // "enabled". On this page an address is what switches persistence on, so offering one that
    // the template never asked for configures a broker against a Redis nobody installed.
    EXPECT_STREQ("", defaults.m_redis_host.asString().c_str());
    EXPECT_EQ(6379, defaults.m_redis_port.asInteger());
}

TEST(XMQ_Settings, applyInitialSetup)
{
    const TemporaryCertificateDirectory certificates;
    const auto                          templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    // Something the setup screen does not ask about, changed first: applying has to put it back
    // to what the template says, not leave it as the configuration being replaced had it.
    CServerLimits serverLimits;
    CQueueLimits  queueLimits;
    settings.limitsControl("get", serverLimits, queueLimits);
    queueLimits.m_max_size = 999;
    settings.limitsControl("set", serverLimits, queueLimits);

    settings.applyInitialSetup(testInitialSetup(), templateFile);

    EXPECT_STREQ("xmq-1884", settings.m_cluster.m_this_node.m_node_name.asString().c_str());
    // The node's own address follows the port MQTT+SSL is now served on. The setup named no address,
    // so it is this machine's - never the template's "localhost", which resolves on every machine
    // and reaches the wrong server on all but one of them.
    EXPECT_EQ(thisHostName() + ":8884", settings.m_cluster.m_this_node.m_host_port.asString());

    EXPECT_TRUE(settings.m_cluster.m_this_node.m_encrypted.asBool());
    EXPECT_EQ(1884, listenerPort(settings, "MQTT"));
    EXPECT_EQ(8884, listenerPort(settings, "MQTT+SSL"));
    // Replaced rather than added to: the listeners the template defines are the only ones left.
    EXPECT_EQ(2U, settings.m_connections.m_listener.size());

    EXPECT_EQ(18884, settings.m_web_service.m_listener_port.asInteger());

    EXPECT_TRUE(settings.m_persistence.m_enabled.asBool());
    EXPECT_STREQ("redis://redis-host:6380", settings.m_persistence.m_redis_uri.asString().c_str());

    EXPECT_EQ(16384, settings.m_queue_limits.m_max_size.asInteger());

    // The accounts are part of what is replaced: what is left is the administrator with the
    // password the setup carried, and the account cluster peers connect as.
    EXPECT_EQ(2U, settings.userManager().getUsers(".*", false).size());
    EXPECT_TRUE(settings.userManager().authenticate("admin", "New-Secret1"));
    EXPECT_FALSE(settings.userManager().authenticate("admin", "admin"));
    EXPECT_TRUE(settings.userManager().isAdministrator("admin"));
    EXPECT_TRUE(settings.userManager().hasUser("cluster"));

    // The previous installation's MQTT clients are not carried over.
    EXPECT_FALSE(settings.userManager().hasUser("user"));
    EXPECT_FALSE(settings.userManager().authenticate("user", "secret"));
}

TEST(XMQ_Settings, applyInitialSetupUsesTheAddressItWasGiven)
{
    const TemporaryCertificateDirectory certificates;
    const auto                          templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    auto setup = testInitialSetup();
    setup.m_node_host = "mqtt-1.example.net";
    settings.applyInitialSetup(setup, templateFile);

    // The address other nodes are told to connect to. Nothing else in the configuration records
    // how this server is reached rather than how it listens.
    EXPECT_EQ("mqtt-1.example.net:8884", settings.m_cluster.m_this_node.m_host_port.asString());
}

TEST(XMQ_Settings, applyInitialSetupIssuesTheNodeCertificate)
{
    const TemporaryCertificateDirectory certificates;
    const auto                          templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    auto setup = testInitialSetup();
    setup.m_node_host = "mqtt-1.example.net";
    settings.applyInitialSetup(setup, templateFile);

    const auto [certificateFile, privateKeyFile] = Settings::nodeKeyFiles();
    ASSERT_TRUE(filesystem::exists(certificateFile));
    ASSERT_TRUE(filesystem::exists(privateKeyFile));

    // Made out to the address the setup gave, which is the whole reason it is issued here: at
    // startup the only name available is the one this machine calls itself.
    const auto description = SelfSignedCertificate::describe(certificateFile);
    EXPECT_NE(String::npos, description.find("CN=mqtt-1.example.net")) << description.c_str();

    // Both listeners start out on it. A node has one identity, and the shipped certificate that
    // every other installation of XMQ also holds is not it.
    EXPECT_EQ(certificateFile.string(), settings.m_connections.m_ssl_keys.m_certfile.asString());
    EXPECT_EQ(privateKeyFile.string(), settings.m_connections.m_ssl_keys.m_keyfile.asString());
    EXPECT_EQ(certificateFile.string(), settings.m_web_service.m_certfile.asString());
    EXPECT_EQ(privateKeyFile.string(), settings.m_web_service.m_keyfile.asString());
}

TEST(XMQ_Settings, applyInitialSetupKeepsThePreviousCertificate)
{
    const TemporaryCertificateDirectory certificates;
    const auto                          templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    settings.applyInitialSetup(testInitialSetup(), templateFile);
    const auto [certificateFile, privateKeyFile] = Settings::nodeKeyFiles();
    const auto replaced = SelfSignedCertificate::describe(certificateFile);

    settings.applyInitialSetup(testInitialSetup(), templateFile);

    // Setting a server up again reissues - and the pair it replaces may be one whose fingerprint
    // was published, or that another node was given, so it is kept rather than removed.
    auto previous = certificateFile;
    previous += ".old";
    ASSERT_TRUE(filesystem::exists(previous));
    EXPECT_EQ(replaced, SelfSignedCertificate::describe(previous));
    EXPECT_NE(replaced, SelfSignedCertificate::describe(certificateFile));
}

TEST(XMQ_Settings, refusesANodeAddressCarryingAPort)
{
    const TemporaryCertificateDirectory certificates;
    const auto                          templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    auto setup = testInitialSetup();
    setup.m_node_host = "mqtt-1.example.net:1883";

    // The ports are asked for separately and appended to this, so an address carrying one of its
    // own would produce an address with two.
    EXPECT_THROW(settings.applyInitialSetup(setup, templateFile), Exception);
}

TEST(XMQ_Settings, applyInitialSetupCreatesAdministrator)
{
    const auto templateFile = writeConfigurationTemplate();

    // A configuration with accounts, but no administrator among them: the setup has to produce
    // one, or it leaves a server nobody can sign in to administer.
    const String withoutAdministrator =
        String(g_testConfig).replace(R"("username": "admin")", R"("username": "operator")");

    Settings settings;
    settings.loadConfiguration(Buffer(withoutAdministrator));
    EXPECT_FALSE(settings.userManager().hasUser("admin"));

    settings.applyInitialSetup(testInitialSetup(), templateFile);

    EXPECT_TRUE(settings.userManager().hasUser("admin"));
    EXPECT_TRUE(settings.userManager().authenticate("admin", "New-Secret1"));
    EXPECT_TRUE(settings.userManager().findUser("admin").m_is_enabled.asBool());
}

TEST(XMQ_Settings, applyInitialSetupTwiceLeavesOneAdministrator)
{
    const auto templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    settings.applyInitialSetup(testInitialSetup(), templateFile);
    auto again = testInitialSetup();
    again.m_admin_password = "Later-Secret1";
    settings.applyInitialSetup(again, templateFile);

    // One account per name, however many times the page is used: an account list that grows a
    // duplicate 'admin' on every run is what a modification saved under a fresh id looks like.
    EXPECT_EQ(2U, settings.userManager().getUsers(".*", false).size());
    EXPECT_TRUE(settings.userManager().authenticate("admin", "Later-Secret1"));
}

TEST(XMQ_Settings, modifyUserWithoutIdChangesTheAccountItNames)
{
    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    const auto accountCount = settings.userManager().getUsers(".*", false).size();

    // The id is what identifies an account; a caller that sends only a username expects the
    // account of that name to change, not a second one to appear beside it.
    CUser user;
    user.m_username = "user";
    user.m_password = "Changed-1!";
    settings.userManager().modifyUser(user);

    EXPECT_EQ(accountCount, settings.userManager().getUsers(".*", false).size());
    EXPECT_TRUE(settings.userManager().authenticate("user", "Changed-1!"));
    EXPECT_FALSE(settings.userManager().authenticate("user", "secret"));
}

TEST(XMQ_Settings, applyInitialSetupWithoutRedis)
{
    const auto templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    auto setup = testInitialSetup();
    setup.m_redis_host = "";
    settings.applyInitialSetup(setup, templateFile);

    EXPECT_FALSE(settings.m_persistence.m_enabled.asBool());
}

TEST(XMQ_Settings, applyInitialSetupRejectsUnusableSettings)
{
    const auto templateFile = writeConfigurationTemplate();

    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    auto withoutPassword = testInitialSetup();
    withoutPassword.m_admin_password = "";
    EXPECT_ANY_THROW(settings.applyInitialSetup(withoutPassword, templateFile));

    auto sharedPorts = testInitialSetup();
    sharedPorts.m_web_service_port = sharedPorts.m_mqtt_port.asInteger();
    EXPECT_ANY_THROW(settings.applyInitialSetup(sharedPorts, templateFile));

    auto privilegedPort = testInitialSetup();
    privilegedPort.m_mqtt_port = 80;
    EXPECT_ANY_THROW(settings.applyInitialSetup(privilegedPort, templateFile));

    auto missingTemplate = testInitialSetup();
    EXPECT_ANY_THROW(settings.applyInitialSetup(
        missingTemplate, DirectoryNames::tempDirectory() / "xmq_no_such.conf.template"));

    // Rejected before anything was written: the configuration is the one it started with.
    EXPECT_STREQ("xmq-1880", settings.m_cluster.m_this_node.m_node_name.asString().c_str());
    EXPECT_EQ(1880, listenerPort(settings, "MQTT"));
}


TEST(XMQ_Settings, createConfigurationFromTemplateBesideIt)
{
    const auto directory = emptyTestDirectory("xmq_create_from_template");
    const auto configurationFile = directory / "xmq_server.conf";

    const Buffer templateText(g_testConfig);
    auto         templateFile = configurationFile;
    templateFile += ".template";
    templateText.saveToFile(templateFile);

    EXPECT_FALSE(Settings::createConfiguration(configurationFile).empty());
    EXPECT_TRUE(filesystem::exists(configurationFile));

    // The template beside the file wins over the installed one, so the configuration is the one
    // this installation says a server should start from.
    Settings settings;
    settings.loadConfiguration(configurationFile);
    EXPECT_EQ(18880, settings.m_web_service.m_listener_port.asInteger());

    // Called again, it leaves the configuration in use alone.
    EXPECT_TRUE(Settings::createConfiguration(configurationFile).empty());
}

TEST(XMQ_Settings, createConfigurationFromBuiltInDefaults)
{
    const auto directory = emptyTestDirectory("xmq_create_from_defaults");
    const auto configurationFile = directory / "xmq_server.conf";
    const auto missingTemplate = directory / "no_such.conf.template";

    EXPECT_STREQ("built-in defaults",
                 Settings::createConfiguration(configurationFile, missingTemplate).c_str());

    // The point of the built-in configuration: a server built from it comes up and serves the
    // interface, so the rest can be corrected there.
    Settings settings;
    settings.loadConfiguration(configurationFile);
    EXPECT_EQ(18883, settings.m_web_service.m_listener_port.asInteger());
    EXPECT_EQ(1883, listenerPort(settings, "MQTT"));
    EXPECT_EQ(8883, listenerPort(settings, "MQTT+SSL"));
    EXPECT_FALSE(settings.m_persistence.m_enabled.asBool());
}

TEST(XMQ_Settings, createsUsersFileWithAdministrator)
{
    const auto directory = emptyTestDirectory("xmq_create_users");
    const auto configurationFile = directory / "xmq_server.conf";
    const auto missingTemplate = directory / "no_such.conf.template";
    Settings::createConfiguration(configurationFile, missingTemplate);

    // Neither the configuration nor a users file carries an account, which is what a fresh
    // installation looks like.
    Settings settings;
    settings.loadConfiguration(configurationFile);

    // A fresh installation: the starting accounts go straight into the database, and no accounts
    // file is written at all - not even one to be renamed a moment later.
    EXPECT_TRUE(filesystem::exists(directory / "xmq_users.db"));
    EXPECT_FALSE(filesystem::exists(directory / "xmq_users.conf"))
        << "an accounts file was created beside the database";
    EXPECT_FALSE(filesystem::exists(directory / "xmq_users.hide"))
        << "an accounts file was created and then renamed, which is churn nobody asked for";

    EXPECT_TRUE(settings.userManager().isAdministrator("admin"));

    // The account exists and has no password, and no password gets in with it - not even the
    // empty one, which is what a client that sent no password at all arrives with. The interface
    // admits it by asking administratorPasswordSet(), and binds itself to the loopback address
    // on the same answer.
    EXPECT_FALSE(settings.administratorPasswordSet());
    EXPECT_FALSE(settings.userManager().authenticate("admin", ""));
    EXPECT_FALSE(settings.userManager().authenticate("admin", "admin"));

    // The node's own cluster entry is built at startup whether or not there is a cluster, and it
    // is built with this account - so a server without one does not start.
    EXPECT_TRUE(settings.userManager().hasUser("cluster"));
    EXPECT_FALSE(settings.userManager().isAdministrator("cluster"));

    // Written out, not just held in memory: the next start has to find it there, and has to read
    // back the same "no password yet" rather than an account it cannot tell from a broken one.
    Settings restarted;
    restarted.loadConfiguration(configurationFile);
    EXPECT_TRUE(restarted.userManager().hasUser("admin"));
    EXPECT_FALSE(restarted.administratorPasswordSet());
}

TEST(XMQ_Settings, initialSetupOffersNoRedisWhenPersistenceIsOff)
{
    const auto directory = emptyTestDirectory("xmq_setup_redis");
    const auto configurationFile = directory / "xmq_server.conf";
    const auto missingTemplate = directory / "no_such.conf.template";
    Settings::createConfiguration(configurationFile, missingTemplate);

    Settings settings;
    settings.loadConfiguration(configurationFile);

    // The starting configuration has persistence off while still carrying an address for it. The
    // page must not offer that address: on this page an address means "switch persistence on",
    // and accepting the defaults would configure a broker against a Redis nobody installed.
    const auto offered = settings.initialSetup(configurationFile);
    EXPECT_STREQ("", offered.m_redis_host.asString().c_str());
    EXPECT_EQ(6379, offered.m_redis_port.asInteger()) << "the port is still offered, so that "
                                                        "typing a host is all it takes";

    // With persistence on, the address is what the template says. Edited in the file rather than
    // through Settings, because initialSetup() reads the template from disk and never consults
    // the configuration in force.
    Buffer templateText;
    templateText.loadFromFile(configurationFile);
    String edited(templateText.c_str());
    edited = edited.replace("\"redis://localhost:6379\"", "\"redis://cache-host:6390\"");
    edited = edited.replace("\"enabled\": false", "\"enabled\": true");
    Buffer(edited).saveToFile(configurationFile);

    const auto offeredWithRedis = settings.initialSetup(configurationFile);
    EXPECT_STREQ("cache-host", offeredWithRedis.m_redis_host.asString().c_str());
    EXPECT_EQ(6390, offeredWithRedis.m_redis_port.asInteger());
}

/**
 * @brief Database accounts keep their IDs and anonymous access setting after restart.
 */
TEST(XMQ_Settings, databaseAccountsKeepTheirIdsAndAnonymousSettingAfterRestart)
{
    const auto directory = emptyTestDirectory("xmq_users_after_restart");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    {
        Settings first;
        first.loadConfiguration(configurationFile);
        CUser temporary;
        temporary.m_username = "temporary";
        temporary.m_password = "Secret#1";
        first.userManager().addUser(temporary);
        first.userManager().removeUser(first.userManager().findUser("temporary"));
    }

    Settings restarted;
    restarted.loadConfiguration(configurationFile);
    const auto adminId = restarted.userManager().findUser("admin").m_id.asInt64();

    CUser added;
    added.m_username = "user";
    added.m_password = "Secret#1";
    restarted.userManager().addUser(added);

    EXPECT_EQ(adminId, restarted.userManager().findUser("admin").m_id.asInt64());
    EXPECT_TRUE(restarted.userManager().isAdministrator("admin"));
    EXPECT_TRUE(restarted.userManager().authenticate("user", "Secret#1"));
    EXPECT_NE(adminId, restarted.userManager().findUser("user").m_id.asInt64());
    const auto groups = restarted.userManager().groupsOf("user");
    EXPECT_NE(groups.end(), ranges::find(groups, "Default"));

    restarted.userManager().allowAnonymous(true);
    Settings reloaded;
    reloaded.loadConfiguration(configurationFile);
    EXPECT_TRUE(reloaded.userManager().isAllowAnonymous());
    EXPECT_EQ(adminId, reloaded.userManager().findUser("admin").m_id.asInt64());
    EXPECT_EQ(restarted.userManager().findUser("user").m_id.asInt64(),
              reloaded.userManager().findUser("user").m_id.asInt64());
}

TEST(XMQ_Settings, initialSetupGivesTheAdministratorAPassword)
{
    const auto directory = emptyTestDirectory("xmq_first_setup");
    const auto configurationFile = directory / "xmq_server.conf";
    const auto missingTemplate = directory / "no_such.conf.template";
    Settings::createConfiguration(configurationFile, missingTemplate);

    Settings settings;
    settings.loadConfiguration(configurationFile);
    ASSERT_FALSE(settings.administratorPasswordSet());

    // The template the setup builds from is the configuration just written, this installation
    // having no installed one to read.
    auto setup = settings.initialSetup(configurationFile);
    setup.m_admin_password = "First-Secret1";
    settings.applyInitialSetup(setup, configurationFile);

    // Which is the whole point of the state: it ends, and it ends here.
    EXPECT_TRUE(settings.administratorPasswordSet());
    EXPECT_TRUE(settings.userManager().authenticate("admin", "First-Secret1"));
    EXPECT_FALSE(settings.userManager().authenticate("admin", ""));

    Settings restarted;
    restarted.loadConfiguration(configurationFile);
    EXPECT_TRUE(restarted.administratorPasswordSet());
}

TEST(XMQ_Settings, resetConfigurationKeepsThePreviousOne)
{
    const auto directory = emptyTestDirectory("xmq_reset");
    const auto configurationFile = directory / "xmq_server.conf";
    const auto missingTemplate = directory / "no_such.conf.template";

    const Buffer unusable(R"({"this": "will not load"})");
    unusable.saveToFile(configurationFile);

    const auto description = Settings::resetConfiguration(configurationFile, missingTemplate);

    auto previousFile = configurationFile;
    previousFile += ".old";
    EXPECT_TRUE(filesystem::exists(previousFile)) << description.c_str();

    Settings settings;
    settings.loadConfiguration(configurationFile);
    EXPECT_EQ(18883, settings.m_web_service.m_listener_port.asInteger());

    // The accounts are in their own file and are not what a reset touches - except that an
    // administrator is created when there are none at all, and a created one has no password.
    EXPECT_TRUE(settings.userManager().hasUser("admin"));
    EXPECT_FALSE(settings.administratorPasswordSet());
}

// Where the accounts live. A fresh installation must need no database and no setting, and two
// brokers configured in different directories must not share accounts without being told to - the
// same rule the users file already follows.
TEST(XMQ_Settings, accountsDefaultToASqliteFileBesideTheConfiguration)
{
    const auto directory = emptyTestDirectory("xmq_user_database");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    Settings settings;
    settings.loadConfiguration(configurationFile);

    const auto expected = DirectoryNames::sqliteUri(directory / "xmq_users.db");
    EXPECT_EQ(expected, String(settings.userDatabaseUri()))
        << "the default belongs beside the configuration it serves";

    EXPECT_EQ(PasswordHash::defaultIterations, settings.passwordIterations())
        << "an unset cost is the built-in one, not zero";
}

TEST(XMQ_Settings, aConfiguredDatabaseIsUsedInsteadOfTheDefault)
{
    const auto directory = emptyTestDirectory("xmq_user_database_configured");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    Buffer configuration;
    configuration.loadFromFile(configurationFile);
    String edited(configuration.c_str());
    // An address on a closed port: refused at once, where a name waited out a resolver timeout.
    edited = edited.replace(R"("allow_anonymous")",
                            R"("database_uri": "postgresql://someone:secret@127.0.0.1:1/accounts?connect_timeout=1",
            "password_iterations": 25000,
            "allow_anonymous")");
    Buffer(edited).saveToFile(configurationFile);

    Settings settings;
    settings.loadConfiguration(configurationFile);

    EXPECT_STREQ("postgresql://someone:secret@127.0.0.1:1/accounts?connect_timeout=1",
                 settings.userDatabaseUri().c_str());
    EXPECT_EQ(25000, settings.passwordIterations());
}

// An installation from before verifiers: the users file holds each password inside a JWT, in the
// open. Loading it converts them, and the conversion has to be invisible to whoever owns the
// account - nobody is asked to choose a new password, because this reading can still see the old
// one and no later reading will.
TEST(XMQ_Settings, anOldUsersFileIsConvertedOnTheOneReadingThatStillSeesThePasswords)
{
    const auto directory = emptyTestDirectory("xmq_password_migration");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    // Written as an older broker wrote it, JWTs and all.
    const String legacyUsers =
        R"({"users":[{"id":1,"username":"admin","password":")" +
        UserManager::makeUserToken("admin", "old-admin-password", "", "", DateTime()) +
        R"(","is_enabled":true,"is_admin":true},)"
        R"({"id":2,"username":"cluster","password":")" +
        UserManager::makeUserToken("cluster", "shared-cluster-secret", "", "", DateTime()) +
        R"(","is_enabled":true,"is_admin":false}]})";
    Buffer(legacyUsers).saveToFile(directory / "xmq_users.conf");

    Settings settings;
    settings.loadConfiguration(configurationFile);

    // The password still works, which is the whole point of converting rather than resetting.
    EXPECT_TRUE(settings.userManager().authenticate("admin", "old-admin-password"));
    EXPECT_FALSE(settings.userManager().authenticate("admin", "not-it"));

    // And it is no longer stored: what is there now is a verifier.
    const auto admin = settings.userManager().findUser("admin");
    EXPECT_TRUE(PasswordHash::isHashed(admin.m_password.asString()))
        << "the account was left in the old form";

    // The cluster secret is the one that cannot become a verifier and nothing else, because this
    // node presents it to its peers. It moves to the configuration on this same reading.
    EXPECT_STREQ("shared-cluster-secret", settings.clusterPassword().c_str());
    EXPECT_TRUE(PasswordHash::verify("shared-cluster-secret",
                                     settings.userManager().findUser("cluster").m_password.asString()))
        << "peers could no longer be checked against the cluster account";
}

// Nothing readable in the file afterwards. The conversion is worth little if the old form is still
// sitting there for the next person who opens it.
TEST(XMQ_Settings, theConvertedUsersFileNoLongerHoldsAnyPassword)
{
    const auto directory = emptyTestDirectory("xmq_password_migration_file");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    const String legacyUsers =
        R"({"users":[{"id":1,"username":"admin","password":")" +
        UserManager::makeUserToken("admin", "a-distinctive-password", "", "", DateTime()) +
        R"(","is_enabled":true,"is_admin":true}]})";
    Buffer(legacyUsers).saveToFile(directory / "xmq_users.conf");

    {
        // Loading is what converts, and writing the file back is part of it: leaving the old form
        // there would keep the password readable and convert again at every start.
        Settings settings;
        settings.loadConfiguration(configurationFile);
    }

    // The file is put aside once its accounts are in the database, so that is where to look - and
    // what it must not hold is the password, whichever name it now goes by.
    const auto asideName = directory / "xmq_users.hide";
    ASSERT_TRUE(filesystem::exists(asideName)) << "the accounts file was not kept";

    Buffer written;
    written.loadFromFile(asideName);
    const String contents(written.c_str());

    EXPECT_EQ(String::npos, contents.find("a-distinctive-password"))
        << "the password is still in the file after conversion";
    EXPECT_NE(String::npos, contents.find("pbkdf2-sha256$"))
        << "the file does not hold a verifier either";
}

// The interface asks for these settings and shows them, so what leaves the broker must not carry
// the password - and what comes back must not overwrite it with the mask it was shown.
TEST(XMQ_Settings, aPasswordInAConnectionUriDoesNotLeaveTheBroker)
{
    const auto directory = emptyTestDirectory("xmq_uri_masking");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    Settings settings;
    settings.loadConfiguration(configurationFile);

    CPersistence persistence;
    persistence.m_redis_uri = "redis://someone:hunter2@cache-host:6379";
    persistence.m_enabled = true;

    const auto afterSet = settings.persistenceControl("set", persistence);
    EXPECT_STREQ("redis://someone:*****@cache-host:6379", afterSet.m_redis_uri.getString())
        << "the password was handed to whoever asked";

    // Sent back as shown, meaning "unchanged". The stored password has to survive that, or every
    // save of an unrelated field would quietly break the connection.
    const auto afterUnchangedSave = settings.persistenceControl("set", afterSet);
    EXPECT_STREQ("redis://someone:*****@cache-host:6379", afterUnchangedSave.m_redis_uri.getString());

    Buffer written;
    written.loadFromFile(configurationFile);
    const String contents(written.c_str());
    EXPECT_NE(String::npos, contents.find("hunter2"))
        << "the password was lost by a save that changed nothing else";
    EXPECT_EQ(String::npos, contents.find("*****"))
        << "the mask was written to the configuration as the password";
}

// These files hold secrets the broker must be able to present - a database password, the cluster
// secret, a bridge's credentials - and were written readable by every account on the machine.
TEST(XMQ_Settings, theFilesItWritesAreNotReadableByEveryone)
{
#ifdef _WIN32
    // Windows has no owner/group/other bits, and std::filesystem::permissions there mostly does
    // nothing while status() reports everything as permitted. Asserting on those bits would be
    // asserting on the standard library's stand-in rather than on what the broker did. Access to
    // these files on Windows is the installer's business, through the ACL it sets.
    GTEST_SKIP() << "file permission bits are a POSIX notion";
#else
    const auto directory = emptyTestDirectory("xmq_file_permissions");
    const auto configurationFile = directory / "xmq_server.conf";
    Settings::createConfiguration(configurationFile, directory / "no_such.conf.template");

    Settings settings;
    settings.loadConfiguration(configurationFile);
    // Anything that makes the broker write both files. Adding an account does.
    CUser user;
    user.m_username = "someone";
    user.m_password = "A-Password1";
    settings.userManager().addUser(user);

    // The account store as well as the configuration. It used to be left out of this loop, and was
    // world-readable on every installation where the broker created it rather than the package -
    // which is now all of them, because the package no longer ships a live one. It holds a password
    // hash per account.
    for (const auto& path: {configurationFile, directory / "xmq_users.db"})
    {
        ASSERT_TRUE(filesystem::exists(path)) << path.string();
        const auto mode = filesystem::status(path).permissions();
        EXPECT_EQ(filesystem::perms::none, mode & filesystem::perms::others_all)
            << path.string() << " is readable by every account on the machine";
        EXPECT_EQ(filesystem::perms::none, mode & filesystem::perms::group_write)
            << path.string() << " is writable by its group";
    }
#endif
}

TEST(XMQ_Settings, publishEventsAreOffUntilAskedFor)
{
    Settings settings;
    settings.loadConfiguration(Buffer(g_testConfig));

    // The rule in one place: publish and ack happen per message, everything else once per session
    // or per subscription. A configuration that says nothing gets that, not one value for all.
    EXPECT_TRUE(settings.eventEnabled(LogSubject::Connect));
    EXPECT_TRUE(settings.eventEnabled(LogSubject::Disconnect));
    EXPECT_TRUE(settings.eventEnabled(LogSubject::Subscribe));
    EXPECT_TRUE(settings.eventEnabled(LogSubject::Unsubscribe));
    EXPECT_FALSE(settings.eventEnabled(LogSubject::Publish));
    EXPECT_FALSE(settings.eventEnabled(LogSubject::Ack));
}

TEST(XMQ_Settings, theEventsBlockOverridesTheDefaults)
{
    // The block added to the same configuration the other tests use, so what is under test is the
    // block and not a configuration written by hand for it.
    const String withEvents = String(g_testConfig).replace(
        R"("logging")", R"("events": {"publish": true, "connect": false}, "logging")");
    ASSERT_NE(g_testConfig, withEvents) << "the test configuration no longer has a logging block";

    Settings settings;
    settings.loadConfiguration(Buffer(withEvents));

    EXPECT_TRUE(settings.eventEnabled(LogSubject::Publish)) << "asked for, and the cost accepted";
    EXPECT_FALSE(settings.eventEnabled(LogSubject::Connect));

    // Not mentioned in the block is not the same as switched off.
    EXPECT_TRUE(settings.eventEnabled(LogSubject::Disconnect));
}
