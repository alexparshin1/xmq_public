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

#include "ServerTests_Suite.h"
#include "TestOptions.h"
#include "TestServers.h"
#include "common/DirectoryNames.h"
#include "server/Extensions/ExtensionHost.h"

#include <filesystem>
#include <fstream>
#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The accounts every test node starts with, in the users-file form the broker migrates from.
///
/// Not in xmq_server.conf any more. Since 0.9.16 an MQTT client is admitted by an extension, and an
/// extension reads the accounts database - so the accounts have to reach that database. The broker
/// fills it from the users file, converting these old readable-password entries into PBKDF2
/// verifiers on the way, which is also the round trip the user-database extension is written
/// against. Accounts left in the configuration are ignored by the broker and say so on stderr.
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

/// A directory of this node's own, under the temporary directory, emptied before it is used.
///
/// Per node and per process: the suites run one after another in one binary, and two nodes sharing
/// a configuration directory would share the accounts database with it - which is exactly the kind
/// of cross-talk that makes one test fail because of what another left behind.
filesystem::path nodeConfigurationDirectory(const string& nodeName)
{
    const auto directory = filesystem::temp_directory_path() /
                           ("xmq_test_node_" + nodeName + "_" + to_string(::getpid()));
    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
    filesystem::create_directories(directory);
    return directory;
}

/// Configures the user-database authenticator beside a node's configuration.
///
/// The same extension the broker ships, pointed at the database Settings creates from this
/// configuration - so the accounts the test declares are the accounts it can connect with. The
/// broker migrates them into that database with its own PBKDF2, and the extension verifies them
/// with its own copy of the algorithm, which is the round trip the extension exists to make.
void writeAuthenticatorConfiguration(const filesystem::path& configurationPath)
{
#ifdef XMQ_TEST_USER_DATABASE_LIBRARY
    const auto fragments = ExtensionHost::configurationDirectoryFor(configurationPath);
    filesystem::create_directories(fragments);

    const auto database = filesystem::path(configurationPath).replace_filename("xmq_users.db");

    ofstream file(fragments / "50-user-database.conf");
    file << R"({"extensions":[{"name":"user-database","library":")"
         << filesystem::path(XMQ_TEST_USER_DATABASE_LIBRARY).generic_string()
         << R"(","enabled":true,"required":false,"settings":{"database":")"
         << DirectoryNames::sqliteUri(database) << R"("}}]})";
#else
    (void) configurationPath;
#endif
}

} // namespace

map<string, SServer, std::less<>> ServerTests_Suite::m_servers;
string                            ServerTests_Suite::m_testSuiteName;
shared_ptr<FileLogEngine>         ServerTests_Suite::m_logEngine;

void ServerTests_Suite::SetUp()
{
    if (const auto testSuitName = testing::UnitTest::GetInstance()->current_test_info()->test_suite_name();
        testSuitName != m_testSuiteName)
    {
        m_testSuiteName = testSuitName;
        stopServers();
    }

    if (m_servers.empty())
    {
        createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true, "primary",
                     "user", "secret", LogPriority::Info, true, bridgesNeeded());
        this_thread::sleep_for(100ms);
    }

    if (TestOptions::m_debug)
    {
        logEngine()->minPriority(LogPriority::Debug);
    }
    else
    {
        logEngine()->minPriority(LogPriority::Info);
    }

    printTitle(testName());
}

void ServerTests_Suite::TearDown()
{
    Test::TearDown();
}

void ServerTests_Suite::TearDownTestSuite()
{
    stopServers();

    // Wait until all message deliveries are processed.
    const DateTime deadline = DateTime::Now() + 10s;
    while (MessageDelivery::queuedOperations() && DateTime::Now() < deadline)
    {
        this_thread::sleep_for(100ms);
    }
}

std::shared_ptr<Server> ServerTests_Suite::server(const std::string& nodeName)
{
    if (m_servers.empty())
    {
        return {};
    }

    if (nodeName.empty())
    {
        return m_servers.begin()->second;
    }

    const auto itor = m_servers.find(nodeName);
    if (itor == m_servers.end())
    {
        return {};
    }
    return itor->second;
}

std::shared_ptr<LogEngine> ServerTests_Suite::logEngine()
{
    if (!m_logEngine)
    {
        m_logEngine = std::make_shared<FileLogEngine>("xmq_unit_tests.log");
        m_logEngine->minPriority(LogPriority::Info);
        m_logEngine->option(LogEngine::Option::TIME, true);
        m_logEngine->option(LogEngine::Option::STDOUT, true);
        m_logEngine->option(LogEngine::Option::MILLISECONDS, true);
        m_logEngine->reset();
    }
    return m_logEngine;
}

std::shared_ptr<Logger> ServerTests_Suite::debugLog(const bool debugMode, const std::string& nodeName)
{
    vector<SServer> nodes;
    if (nodeName.empty())
    {
        for (const auto& node: m_servers | views::values)
        {
            nodes.push_back(node);
        }
    }
    else
    {
        if (const auto _server = server(nodeName))
        {
            nodes.push_back(_server);
        }
        else
        {
            return {};
        }
    }

    for (const auto& node: nodes)
    {
        if (debugMode)
        {
            node->getLogEngine()->minPriority(LogPriority::Debug);
        }
        else
        {
            node->getLogEngine()->minPriority(LogPriority::Info);
        }
    }
    const auto firstNode = *nodes.begin();
    return std::make_shared<Logger>(*firstNode->getLogEngine());
}

string ServerTests_Suite::testName()
{
    return testing::UnitTest::GetInstance()->current_test_info()->name();
}

void ServerTests_Suite::printTitle(const string_view title)
{
    stringstream titleStream;

    const auto padding = (80 - title.length()) / 2;
#ifdef _WIN32
    const string lineChar = "-";
#else
    const string lineChar = "─";
#endif

    for (size_t i = 0; i < padding; ++i)
    {
        titleStream << lineChar;
    }

    titleStream << "[" << title << "]";

    if (title.length() % 2)
    {
        titleStream << lineChar;
    }

    for (size_t i = 0; i < padding; ++i)
    {
        titleStream << lineChar;
    }

    COUT(titleStream.str());
    // const Logger logger(*logEngine());
    // logger.info(format("Server suite unit test: {}", title));
}

SServer ServerTests_Suite::createServer(const uint16_t listenerPortTcp, const uint16_t listenerPortSsl, const uint16_t servicePortTcp,
                                        const bool cleanStart, const std::string& nodeName,
                                        const std::string& userName, const std::string& password,
                                        const LogPriority minLogLevel, const bool persistence, const bool enableBridges)
{
    static const String testConfig = R"({
        "connections": {
            "ssl_keys": {
                "cafile": "",
                "keyfile": "${ProgramCerts}/node.key",
                "certfile": "${ProgramCerts}/node.crt",
                "verify_depth": 0,
            },
            "listener": [
                {
                    "id": 1,
                    "name": "Not Encrypted",
                    "protocol": "MQTT",
                    "bind_ip": "0.0.0.0",
                    "port": 1883,
                    "threads": 4,
                    "enable": true
                },
                {
                    "id": 2,
                    "name": "Encrypted",
                    "protocol": "MQTT+SSL",
                    "bind_ip": "0.0.0.0",
                    "port": 8883,
                    "threads": 1,
                    "enable": true
                }
            ]
        },
        "server_limits": {
            "send_threads": 8,
            "receive_threads": 8,
            "max_topic_alias": 128
        },
        "queue_limits": {
            "max_size": 16384,
            "max_inflight_messages": 128
        },
        "authentication": {
            "allow_anonymous": false
        },
        "persistence": {
            "enabled": true,
            "redis_uri": ")" + TestServers::redisUri() + R"(",
            "clean_start": true,
            "max_threads": 8,
            "max_redis_connections": 2
        },
        "logging": {
            "log_to": "xmq_server.log",
            "min_log_level": "INFO",
            "log_level_available": ["DEBUG","INFO","NOTICE","WARNING","ERROR","PANIC"],
            "log_level": {
                "ack": "INFO",
                "connect": "INFO",
                "disconnect": "INFO",
                "publish": "DEBUG",
                "subscribe": "DEBUG",
                "unsubscribe": "INFO",
                "server_connections": "INFO",
                "server_events": "INFO",
                "cluster_connections": "INFO",
                "cluster_events": "INFO"
            },
        },
        "web_service": {
            "listener_port": 18883
        },
        "bridges": [
          {
            "id": 1,
            "node_name": "other",
            "enabled": true,
            "host_port": "localhost:1886",
            "username": "${USER}",
            "password": "${PASSWORD}",
            "client_id": "eastside_monitor",
            "encrypted": false,
            "clean_session": true,
            "mode": "inout",
            "topics": [{"direction": "inout", "pattern": "#", "qos": 1}]
          },
          {
            "id": 2,
            "node_name": "primary",
            "enabled": true,
            "host_port": "localhost:1880",
            "username": "${USER}",
            "password": "${PASSWORD}",
            "client_id": "eastside_monitor",
            "encrypted": false,
            "clean_session": true,
            "mode": "cluster",
            "topics": [{"direction": "inout", "pattern": "#", "qos": 1}]
          },
          {
            "id": 3,
            "node_name": "secondary",
            "enabled": true,
            "host_port": "localhost:1886",
            "username": "${USER}",
            "password": "${PASSWORD}",
            "client_id": "eastside_monitor",
            "encrypted": false,
            "clean_session": true,
            "mode": "cluster",
            "topics": [{"direction": "inout", "pattern": "#", "qos": 1}]
          },
          {
            "id": 3,
            "node_name": "third",
            "enabled": true,
            "host_port": "localhost:1881",
            "username": "${USER}",
            "password": "${PASSWORD}",
            "client_id": "westside_monitor",
            "encrypted": false,
            "clean_session": true,
            "mode": "cluster",
            "topics": [{"direction": "inout", "pattern": "#", "qos": 1}]
          }
        ],
        "cluster": {
            "this_node": {
                "node_name": "${NODE_NAME}",
                "host_port": "localhost:1883"
            },
            "nodes": []
        }
    })";

    if (m_servers.contains(nodeName))
    {
        throw Exception("Node with the same name already exists.");
    }

    checkPortIsFree(listenerPortTcp);
    checkPortIsFree(listenerPortSsl);
    checkPortIsFree(servicePortTcp);

    auto settings = make_shared<Settings>();
    try
    {
        const auto digestedTestConfig = testConfig
                                            .replace(R"(\$\{NODE_NAME\})", nodeName)
                                            .replace(R"(\$\{USER\})", userName)
                                            .replace(R"(\$\{PASSWORD\})", password);

        // Written to a directory of its own rather than loaded from a buffer, because since 0.9.16
        // an MQTT client is admitted by an extension or not at all: the broker no longer falls back
        // to its own accounts. A test node therefore needs what a real one needs - a configuration
        // with a path, so the accounts land in a database beside it, and an authenticator
        // configured next to that. Without the path there is no database and nothing to
        // authenticate against, and every test that connects would be refused.
        const auto nodeDirectory = nodeConfigurationDirectory(nodeName);
        const auto configurationPath = nodeDirectory / "xmq_server.conf";
        Buffer(digestedTestConfig).saveToFile(configurationPath);
        writeAuthenticatorConfiguration(configurationPath);

        const auto usersPath = nodeDirectory / "xmq_users.conf";
        Buffer(String(testUsers)).saveToFile(usersPath);

        settings->loadConfiguration(configurationPath, usersPath);

        settings->m_connections.m_listener[0].m_port = listenerPortTcp;
        settings->m_connections.m_listener[1].m_port = listenerPortSsl;
        settings->m_web_service.m_listener_port = servicePortTcp;

        // Off unless the suite under test is about bridges. The configuration above declares three,
        // two of them pointing at port 1886, which only exists while the cluster tests run - so for
        // every other suite they retried for the whole run, filled the logs with "Server not
        // available", and, worse, had their connection attempts counted by the broker. That is what
        // made SysTopicsTests.SendReceive_Messages_Connect see three messages where it expected two.
        if (!enableBridges)
        {
            for (auto& bridge: settings->m_bridges)
            {
                bridge.m_enabled = false;
            }
        }

        settings->m_persistence.m_clean_start = cleanStart;
        if (persistence)
        {
            if (!settings->m_persistence.m_redis_uri.asString().empty())
            {
                settings->m_persistence.m_enabled = true;
            }
        }
        else
        {
            // Run entirely in memory: no Redis connection at all, so the code paths that
            // guard on a null storage are actually exercised.
            settings->m_persistence.m_redis_uri = "";
            settings->m_persistence.m_enabled = false;
        }

        // Create default cluster settings:
        settings->m_cluster.m_this_node.m_node_name = nodeName;
        settings->m_cluster.m_this_node.m_host_port = "localhost:" + to_string(listenerPortSsl ? listenerPortSsl : listenerPortTcp);
        settings->m_cluster.m_this_node.m_encrypted = listenerPortSsl != 0;
        settings->m_cluster.m_nodes.clear();

        CServerNode thisNode;
        thisNode.m_node_name = nodeName;
        stringstream str;
        str << "localhost:" << (listenerPortSsl ? listenerPortSsl : listenerPortTcp);
        thisNode.m_host_port = str.str();
        thisNode.m_encrypted = listenerPortSsl != 0;
        settings->m_cluster.m_nodes.push_back(thisNode);

        settings->setLogPriority(minLogLevel);

        // The broker itself creates this directory before it opens its log (see makeLogEngine() and
        // ServerController), and the tests have to do the same rather than inherit it: on a machine
        // where XMQ has never been installed - a fresh FreeBSD checkout, say - nothing else has made
        // it, and every server test then fails in SetUp() with an unopenable log file.
        filesystem::create_directories(DirectoryNames::logsDirectory());

        auto logEngine = make_shared<FileLogEngine>(DirectoryNames::logsDirectory().string() + string("/xmq_server-test-") + to_string(listenerPortTcp) + ".log");
        logEngine->option(LogEngine::Option::STDOUT, true);
        logEngine->option(LogEngine::Option::MILLISECONDS, true);
        logEngine->minPriority(minLogLevel);

        auto server = make_shared<Server>(settings, logEngine, LogPriority::Debug);

        m_servers[nodeName] = server;

        return server;
    }
    catch (const Exception& e)
    {
        CERR(e.what());
        throw;
    }
}

void ServerTests_Suite::checkPortIsFree(const uint16_t port)
{
    if (port == 0)
    {
        return;
    }

    TCPSocket probe;
    try
    {
        probe.open(Host("localhost", port), Socket::OpenMode::CONNECT, false, 250ms);
    }
    catch (const Exception&)
    {
        // Nothing accepted the connection, the port is free.
        return;
    }
    probe.close();
    throw Exception(format("TCP port {} is already in use - another xmq_unit_tests or xmq server instance is likely still running. "
                           "Stop it (e.g. 'pkill xmq_unit_tests') before running the tests.",
                           port));
}

void ServerTests_Suite::stopServers()
{
    for (auto& server: m_servers | views::values)
    {
        server->stopServer();
        server.reset();
    }
    m_servers.clear();
}
