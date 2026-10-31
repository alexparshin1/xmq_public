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

#include "server/Settings/Settings.h"

#include <gtest/gtest.h>

#include <filesystem>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

String configurationText(const int mqttPort = 18886, const String& extra = {})
{
    String text = R"({
        "connections": {
            "listener": [{"id": 1, "name": "Default", "protocol": "MQTT", "bind_ip": "127.0.0.1",
                          "port": __PORT__, "threads": 1, "enable": true}],
            "ssl_keys": {"cafile": "", "keyfile": "${ProgramCerts}/node.key",
                         "certfile": "${ProgramCerts}/node.crt", "verify_depth": 0}
        },
        "server_limits": {"send_threads": 1, "receive_threads": 1, "max_topic_alias": 8},
        "queue_limits": {"max_size": 128, "max_inflight_messages": 16__QUEUE_EXTRA__},
        "authentication": {"allow_anonymous": false},
        "persistence": {"redis_uri": "redis://localhost:6379", "enabled": false},
        "logging": {"log_to": "xmq_server.log", "min_log_level": "INFO", "log_level": {
            "ack": "INFO", "connect": "INFO", "disconnect": "INFO", "publish": "INFO",
            "subscribe": "INFO", "unsubscribe": "INFO", "server_connections": "INFO",
            "server_events": "INFO", "cluster_connections": "INFO", "cluster_events": "INFO"}},
        "web_service": {"listener_port": 0},
        "bridges": [],
        "cluster": {"this_node": {"node_name": "check-node", "host_port": "localhost:8883"}}
    })";
    text = text.replace("__PORT__", String(to_string(mqttPort)));
    text = text.replace("__QUEUE_EXTRA__", extra);
    return text;
}

int processId()
{
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

filesystem::path testDirectory()
{
    const auto directory = filesystem::temp_directory_path() /
                           ("xmq_configuration_" + to_string(processId()));
    filesystem::remove_all(directory);
    filesystem::create_directories(directory);
    return directory;
}

} // namespace

/**
 * A check has to work before a restart, which is the moment it is for: it must not open a
 * database, create an account or rewrite the file it was given.
 */
TEST(XMQ_ConfigurationFile, zeroLeaseSecondsMeansTheDefaultAndANegativeOneIsRefused)
{
    const auto directory = testDirectory();
    const auto path = directory / "xmq_server.conf";
    Strings    warnings;

    auto text = configurationText();
    text = text.replace(R"("cluster": {)", R"("cluster": {"lease_seconds": 0, "links": 0, )");
    Buffer(text).saveToFile(path);
    const auto leaseError = Settings::checkConfiguration(path, warnings);
    EXPECT_TRUE(leaseError.empty()) << leaseError.c_str();

    text = text.replace(R"("lease_seconds": 0)", R"("lease_seconds": -1)");
    Buffer(text).saveToFile(path);
    const auto error = Settings::checkConfiguration(path, warnings);
    EXPECT_NE(error.find("cluster.lease_seconds"), String::npos) << error.c_str();

    filesystem::remove_all(directory);
}

TEST(XMQ_ConfigurationFile, checkConfigurationNamesWhatIsWrong)
{
    const auto directory = testDirectory();
    const auto path = directory / "xmq_server.conf";

    Buffer(configurationText()).saveToFile(path);

    Strings warnings;
    EXPECT_TRUE(Settings::checkConfiguration(path, warnings).empty());
    EXPECT_TRUE(warnings.empty());

    Buffer(configurationText(99999)).saveToFile(path);
    const auto error = Settings::checkConfiguration(path, warnings);
    EXPECT_NE(error.find("connections.listener[0].port"), String::npos) << error.c_str();

    Buffer(configurationText(18886, R"(, "max_sizee": 1)")).saveToFile(path);
    EXPECT_TRUE(Settings::checkConfiguration(path, warnings).empty());
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings.front().find("queue_limits.max_sizee"), String::npos) << warnings.front().c_str();

    filesystem::remove_all(directory);
}

/**
 * A configuration must not be written that a start would refuse. The check on load is what stops
 * a bad file from running; the same check on save is what keeps it from being written at all.
 */
TEST(XMQ_ConfigurationFile, saveRefusesAValueTheLoadWouldRefuse)
{
    const auto directory = testDirectory();
    const auto path = directory / "xmq_server.conf";
    const auto usersPath = directory / "xmq_users.conf";

    Buffer(configurationText()).saveToFile(path);

    Settings settings;
    settings.loadConfiguration(path, usersPath);

    Buffer before;
    before.loadFromFile(path);

    settings.m_connections.m_listener[0].m_port = 99999;
    EXPECT_THROW(
        settings.setAccountDatabaseUri("sqlite3://localhost/" + (directory / "xmq_users.db").string()),
        invalid_argument);

    // Nothing was persisted: the file is byte-for-byte what was loaded.
    Buffer after;
    after.loadFromFile(path);
    EXPECT_EQ(String(before), String(after));

    filesystem::remove_all(directory);
}

/**
 * The Settings screen offers Clean start beside the storage it applies to, and what it saves has to
 * be what the next start reads - the screen used to show the value without a way to set it.
 */
TEST(XMQ_ConfigurationFile, theSettingsScreenSavesCleanStart)
{
    const auto directory = testDirectory();
    const auto path = directory / "xmq_server.conf";
    const auto usersPath = directory / "xmq_users.conf";

    Buffer(configurationText()).saveToFile(path);

    Settings settings;
    settings.loadConfiguration(path, usersPath);
    ASSERT_FALSE(settings.m_persistence.m_clean_start.asBool());

    auto values = settings.nodeSettings();
    values.m_clean_start = true;
    const auto rollback = settings.saveNodeSettings(values);

    {
        Settings reloaded;
        reloaded.loadConfiguration(path, usersPath);
        EXPECT_TRUE(reloaded.m_persistence.m_clean_start.asBool())
            << "the value the screen saved is not what the next start reads";
    }

    // The rollback the save hands back is the way out of a failed apply, and it takes this with it.
    rollback();
    Settings restored;
    restored.loadConfiguration(path, usersPath);
    EXPECT_FALSE(restored.m_persistence.m_clean_start.asBool());

    filesystem::remove_all(directory);
}

/**
 * When the configured file cannot be read, the copy the last successful start kept is what the
 * broker runs on - and a later save must go to the configured file, not to the copy.
 */
TEST(XMQ_ConfigurationFile, lastGoodCopyIsReadAndTheConfiguredPathIsKept)
{
    const auto directory = testDirectory();
    const auto path = directory / "xmq_server.conf";
    const auto usersPath = directory / "xmq_users.conf";

    Buffer(configurationText()).saveToFile(path);

    {
        Settings settings;
        settings.loadConfiguration(path, usersPath);
        ASSERT_EQ(settings.keepLastGoodConfiguration(), "");
    }

    const auto lastGood = Settings::lastGoodConfigurationPath(path);
    ASSERT_TRUE(filesystem::exists(lastGood)) << lastGood.string();

    // What a hand edit gone wrong looks like: a parse error rather than a wrong value.
    Buffer("{\n  \"connections\": {\n").saveToFile(path);

    Settings fallback;
    fallback.loadConfigurationFrom(path, lastGood, usersPath);
    EXPECT_EQ(fallback.configurationPath(), path);
    EXPECT_FALSE(fallback.startupWarning().empty());
    EXPECT_EQ(fallback.m_connections.m_listener[0].m_port.asInteger(), 18886);

    filesystem::remove_all(directory);
}
