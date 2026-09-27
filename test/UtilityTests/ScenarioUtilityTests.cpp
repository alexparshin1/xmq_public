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

#include "scenario/ScenarioEngine.h"
#include "service/CTestScenario.h"
#include "utilities/Scenario.h"
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

// Restores the process current directory on scope exit, so one test's chdir() can't affect another.
class CurrentDirectoryGuard
{
public:
    explicit CurrentDirectoryGuard(const filesystem::path& newDirectory)
    {
        filesystem::current_path(newDirectory);
    }

    ~CurrentDirectoryGuard()
    {
        filesystem::current_path(m_previousDirectory);
    }

    CurrentDirectoryGuard(const CurrentDirectoryGuard&) = delete;
    CurrentDirectoryGuard& operator=(const CurrentDirectoryGuard&) = delete;

private:
    filesystem::path m_previousDirectory = filesystem::current_path();
};

filesystem::path makeEmptyTempDirectory(const string& name)
{
    auto dir = filesystem::temp_directory_path() / ("xmq_scenario_utility_tests_" + name);
    filesystem::remove_all(dir);
    filesystem::create_directories(dir);
    return dir;
}

void writeFile(const filesystem::path& path)
{
    filesystem::create_directories(path.parent_path());
    ofstream(path) << "{}";
}

// A CTestScenario pre-loaded with sentinel values, so tests can tell an override
// apart from a field that was simply left at its zero-initialized default.
CTestScenario presetScenario()
{
    CTestScenario scenario;
    scenario.m_server.m_hostname = "preset-host";
    scenario.m_server.m_port = 9999;
    scenario.m_server.m_username = "preset-user";
    scenario.m_server.m_password = "preset-password";
    scenario.m_parameters.m_payload_size = 111;
    scenario.m_parameters.m_publish_rate = 222;
    scenario.m_parameters.m_message_count = 333;
    scenario.m_parameters.m_duration_sec = 444;
    scenario.m_parameters.m_connection_rate = 555;
    scenario.m_parameters.m_keep_alive_sec = 666;
    scenario.m_publishers.m_qos = 2;
    scenario.m_subscribers.m_qos = 2;
    scenario.m_publishers.m_protocol_version = 4;
    scenario.m_subscribers.m_protocol_version = 4;
    scenario.m_publishers.m_id_prefix = "preset-publisher-";
    scenario.m_subscribers.m_id_prefix = "preset-subscriber-";
    return scenario;
}

} // namespace

/**
 * Every command-line override maps onto the expected scenario field.
 */
TEST(ScenarioUtilityTests, OverrideScenarioParametersAppliesAllOverrides)
{
    const Scenario scenario({"xmq_scn",
                             "--host", "10.0.0.5",
                             "--port", "1885",
                             "--username", "bob",
                             "--password", "secret",
                             "--payload-size", "64",
                             "--publish-rate", "500",
                             "--publish-count", "1000",
                             "--duration", "30",
                             "--connection-rate", "200",
                             "--keep-alive", "45",
                             "--qos", "1",
                             "--protocol-version", "5",
                             "--id-prefix", "host1-"});

    auto testScenario = presetScenario();
    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_server.m_hostname.asString(), "10.0.0.5");
    EXPECT_EQ(testScenario.m_server.m_port.asInteger(), 1885);
    EXPECT_EQ(testScenario.m_server.m_username.asString(), "bob");
    EXPECT_EQ(testScenario.m_server.m_password.asString(), "secret");
    EXPECT_EQ(testScenario.m_parameters.m_payload_size.asInteger(), 64);
    EXPECT_EQ(testScenario.m_parameters.m_publish_rate.asInteger(), 500);
    EXPECT_EQ(testScenario.m_parameters.m_message_count.asInteger(), 1000);
    EXPECT_EQ(testScenario.m_parameters.m_duration_sec.asInteger(), 30);
    EXPECT_EQ(testScenario.m_parameters.m_connection_rate.asInteger(), 200);
    EXPECT_EQ(testScenario.m_parameters.m_keep_alive_sec.asInteger(), 45);
    EXPECT_EQ(testScenario.m_publishers.m_qos.asInteger(), 1);
    EXPECT_EQ(testScenario.m_subscribers.m_qos.asInteger(), 1);
    EXPECT_EQ(testScenario.m_publishers.m_protocol_version.asInteger(), 5);
    EXPECT_EQ(testScenario.m_subscribers.m_protocol_version.asInteger(), 5);
    EXPECT_EQ(testScenario.m_publishers.m_id_prefix.asString(), "host1-preset-publisher-");
    EXPECT_EQ(testScenario.m_subscribers.m_id_prefix.asString(), "host1-preset-subscriber-");
}

/**
 * Regression test: with no overrides on the command line, every scenario-file value must
 * survive untouched. This is the bug that was fixed earlier - sptk::CommandLine stores
 * parameter defaults (e.g. host=localhost, qos=0) in the same map as parsed values, so a
 * naive hasOption() check would silently overwrite scenario-file values with those defaults.
 */
TEST(ScenarioUtilityTests, OverrideScenarioParametersLeavesScenarioUntouchedWithoutOverrides)
{
    const Scenario scenario({"xmq_scn", "--disable-clean-session"});

    auto original = presetScenario();
    auto testScenario = original;
    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_server.m_hostname.asString(), original.m_server.m_hostname.asString());
    EXPECT_EQ(testScenario.m_server.m_port.asInteger(), original.m_server.m_port.asInteger());
    EXPECT_EQ(testScenario.m_server.m_username.asString(), original.m_server.m_username.asString());
    EXPECT_EQ(testScenario.m_server.m_password.asString(), original.m_server.m_password.asString());
    EXPECT_EQ(testScenario.m_parameters.m_payload_size.asInteger(), original.m_parameters.m_payload_size.asInteger());
    EXPECT_EQ(testScenario.m_parameters.m_publish_rate.asInteger(), original.m_parameters.m_publish_rate.asInteger());
    EXPECT_EQ(testScenario.m_parameters.m_message_count.asInteger(), original.m_parameters.m_message_count.asInteger());
    EXPECT_EQ(testScenario.m_parameters.m_duration_sec.asInteger(), original.m_parameters.m_duration_sec.asInteger());
    EXPECT_EQ(testScenario.m_parameters.m_connection_rate.asInteger(), original.m_parameters.m_connection_rate.asInteger());
    EXPECT_EQ(testScenario.m_parameters.m_keep_alive_sec.asInteger(), original.m_parameters.m_keep_alive_sec.asInteger());
    EXPECT_EQ(testScenario.m_publishers.m_qos.asInteger(), original.m_publishers.m_qos.asInteger());
    EXPECT_EQ(testScenario.m_subscribers.m_qos.asInteger(), original.m_subscribers.m_qos.asInteger());
    EXPECT_EQ(testScenario.m_publishers.m_protocol_version.asInteger(), original.m_publishers.m_protocol_version.asInteger());
    EXPECT_EQ(testScenario.m_subscribers.m_protocol_version.asInteger(), original.m_subscribers.m_protocol_version.asInteger());
    EXPECT_EQ(testScenario.m_publishers.m_id_prefix.asString(), original.m_publishers.m_id_prefix.asString());
    EXPECT_EQ(testScenario.m_subscribers.m_id_prefix.asString(), original.m_subscribers.m_id_prefix.asString());
}

/**
 * --keep-alive overrides the scenario-wide keep_alive_sec, used for both publishers and
 * subscribers since ScenarioEngine builds a single shared ConnectParameters for both groups.
 */
TEST(ScenarioUtilityTests, KeepAliveOverrideAppliesToScenarioParameters)
{
    const Scenario scenario({"xmq_scn", "--keep-alive", "300"});

    auto testScenario = presetScenario();

    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_parameters.m_keep_alive_sec.asInteger(), 300);
}

/**
 * --qos overrides both publishers and subscribers QoS together.
 */
TEST(ScenarioUtilityTests, QosOverrideAppliesToPublishersAndSubscribers)
{
    const Scenario scenario({"xmq_scn", "--qos", "2"});

    auto testScenario = presetScenario();
    testScenario.m_publishers.m_qos = 0;
    testScenario.m_subscribers.m_qos = 0;

    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_publishers.m_qos.asInteger(), 2);
    EXPECT_EQ(testScenario.m_subscribers.m_qos.asInteger(), 2);
}

/**
 * --protocol-version overrides both publishers and subscribers protocol version together.
 */
TEST(ScenarioUtilityTests, ProtocolVersionOverrideAppliesToPublishersAndSubscribers)
{
    const Scenario scenario({"xmq_scn", "--protocol-version", "3"});

    auto testScenario = presetScenario();
    testScenario.m_publishers.m_protocol_version = 5;
    testScenario.m_subscribers.m_protocol_version = 5;

    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_publishers.m_protocol_version.asInteger(), 3);
    EXPECT_EQ(testScenario.m_subscribers.m_protocol_version.asInteger(), 3);
}

/**
 * --id-prefix is prepended to each group's existing id_prefix, not a straight replace like every
 * other override: publishers and subscribers keep their own scenario-file prefixes so client IDs
 * stay distinguishable within a single host, while the CLI value adds a per-host tag in front of
 * both - the actual point of this option is letting the same scenario run unmodified from several
 * hosts against one broker without colliding client IDs across hosts.
 */
TEST(ScenarioUtilityTests, IdPrefixOverrideIsPrependedNotReplaced)
{
    const Scenario scenario({"xmq_scn", "--id-prefix", "host1-"});

    auto testScenario = presetScenario();
    testScenario.m_publishers.m_id_prefix = "test-publisher-";
    testScenario.m_subscribers.m_id_prefix = "test-subscriber-";

    scenario.overrideScenarioParameters(testScenario);

    EXPECT_EQ(testScenario.m_publishers.m_id_prefix.asString(), "host1-test-publisher-");
    EXPECT_EQ(testScenario.m_subscribers.m_id_prefix.asString(), "host1-test-subscriber-");
}

TEST(ScenarioUtilityTests, ResolveScenarioFileReturnsAbsolutePathAsIs)
{
    // Built from temp_directory_path() rather than spelled out as "/does/not/...": a leading slash
    // makes a path absolute on Linux, but on Windows it is only root-relative - it carries no drive
    // letter - so is_absolute() is false there and the path would not take the branch under test.
    // The path still doesn't have to exist.
    const filesystem::path absolutePath = filesystem::temp_directory_path() / "does" / "not" / "need" / "to" / "exist" / "scenario.json";

    EXPECT_EQ(Scenario::resolveScenarioFile(absolutePath, "unused-executable-dir").string(), absolutePath.string());
}

/**
 * A relative path found in the current directory wins, without even looking at the installed
 * scenario directory (which isn't created by this test, so a fallback lookup would fail).
 */
TEST(ScenarioUtilityTests, ResolveScenarioFileFindsFileInCurrentDirectory)
{
    const auto tempDir = makeEmptyTempDirectory("cwd");
    writeFile(tempDir / "my" / "own" / "scenario.json");

    {
        const CurrentDirectoryGuard guard(tempDir);

        const auto resolved = Scenario::resolveScenarioFile("my/own/scenario.json", tempDir / "executable-dir-does-not-exist");

        EXPECT_EQ(resolved, filesystem::path("my") / "own" / "scenario.json");
    }

    // Outside the guard's scope, so the current directory is no longer inside tempDir: Windows
    // refuses to remove a directory that is any process's current directory, while Linux allows it.
    filesystem::remove_all(tempDir);
}

/**
 * When not found in the current directory, fall back to the installed scenario directory
 * (${CMAKE_INSTALL_PREFIX}/share/xmq), resolved as ../share/xmq relative to the executable directory.
 */
TEST(ScenarioUtilityTests, ResolveScenarioFileFallsBackToInstalledDirectory)
{
    const auto tempDir = makeEmptyTempDirectory("installed");
    const auto executableDirectory = tempDir / "bin";
    filesystem::create_directories(executableDirectory);
    const auto installedScenarioFile = tempDir / "share" / "xmq" / "Basic" / "scenario.json";
    writeFile(installedScenarioFile);

    // Guard against the file accidentally being found relative to the current directory too.
    const auto emptyCwd = makeEmptyTempDirectory("installed-cwd");

    {
        const CurrentDirectoryGuard guard(emptyCwd);

        const auto resolved = Scenario::resolveScenarioFile("Basic/scenario.json", executableDirectory);

        EXPECT_TRUE(filesystem::exists(resolved));
        EXPECT_TRUE(filesystem::equivalent(resolved, installedScenarioFile));
    }

    // Removed only once the guard has restored the current directory - see the note in
    // ResolveScenarioFileFindsFileInCurrentDirectory.
    filesystem::remove_all(tempDir);
    filesystem::remove_all(emptyCwd);
}

TEST(ScenarioUtilityTests, ResolveScenarioFileThrowsWhenNotFoundAnywhere)
{
    const auto tempDir = makeEmptyTempDirectory("missing");

    {
        const CurrentDirectoryGuard guard(tempDir);

        EXPECT_THROW((void) Scenario::resolveScenarioFile("does-not-exist.json", tempDir / "bin"), Exception);
    }

    // Removed only once the guard has restored the current directory - see the note in
    // ResolveScenarioFileFindsFileInCurrentDirectory.
    filesystem::remove_all(tempDir);
}

TEST(ScenarioUtilityTests, ResolveBindInterfacesAcceptsCommaSeparatedList)
{
    const auto addresses = Scenario::resolveBindInterfaces("10.1.1.24,10.1.1.100");

    ASSERT_EQ(addresses.size(), 2u);
    EXPECT_EQ(addresses[0], "10.1.1.24");
    EXPECT_EQ(addresses[1], "10.1.1.100");
}

TEST(ScenarioUtilityTests, ResolveBindInterfacesRejectsInvalidAddressInList)
{
    EXPECT_THROW((void) Scenario::resolveBindInterfaces("10.1.1.1,not-an-ip"), Exception);
}

/**
 * The loopback interface (127.0.0.1/8) is present on every machine that can run this
 * test suite, so it's a safe, environment-independent way to exercise mask matching.
 */
TEST(ScenarioUtilityTests, ResolveBindInterfacesMatchesLoopbackByMask)
{
    const auto addresses = Scenario::resolveBindInterfaces("127.0.0.1/8");

    ASSERT_FALSE(addresses.empty());
    EXPECT_NE(std::find(addresses.begin(), addresses.end(), String("127.0.0.1")), addresses.end());
}

TEST(ScenarioUtilityTests, ResolveBindInterfacesThrowsWhenNoInterfaceMatchesMask)
{
    // TEST-NET-1 (RFC 5737): reserved for documentation, never assigned to a real interface.
    EXPECT_THROW((void) Scenario::resolveBindInterfaces("192.0.2.1/32"), Exception);
}

TEST(ScenarioUtilityTests, ResolveBindInterfacesRejectsOutOfRangePrefixLength)
{
    EXPECT_THROW((void) Scenario::resolveBindInterfaces("10.1.1.1/99"), Exception);
}

TEST(ScenarioUtilityTests, ResolveBindInterfacesRejectsInvalidMaskAddress)
{
    EXPECT_THROW((void) Scenario::resolveBindInterfaces("not-an-ip/8"), Exception);
}

/**
 * Sub-directories of the installed scenario directory are groups; '*.json' files directly
 * inside a group are listed, sorted, as paths that can be passed straight to --scenario.
 */
TEST(ScenarioUtilityTests, ListScenariosGroupsByImmediateSubDirectory)
{
    const auto tempDir = makeEmptyTempDirectory("list-scenarios");
    const auto executableDirectory = tempDir / "bin";
    filesystem::create_directories(executableDirectory);
    const auto scenarioDirectory = tempDir / "share" / "xmq";
    writeFile(scenarioDirectory / "Basic" / "Fan-Out.json");
    writeFile(scenarioDirectory / "Basic" / "Connections.json");
    writeFile(scenarioDirectory / "Enterprise" / "Fan-In.json");

    const auto groups = Scenario::listScenarios(executableDirectory);

    ASSERT_EQ(groups.size(), 2u);

    const auto basic = groups.find("Basic");
    ASSERT_NE(basic, groups.end());
    ASSERT_EQ(basic->second.size(), 2u);
    EXPECT_EQ(basic->second[0], (filesystem::path("Basic") / "Connections.json").string());
    EXPECT_EQ(basic->second[1], (filesystem::path("Basic") / "Fan-Out.json").string());

    const auto enterprise = groups.find("Enterprise");
    ASSERT_NE(enterprise, groups.end());
    ASSERT_EQ(enterprise->second.size(), 1u);
    EXPECT_EQ(enterprise->second[0], (filesystem::path("Enterprise") / "Fan-In.json").string());

    filesystem::remove_all(tempDir);
}

/**
 * Non-'.json' files and files directly under the scenario directory (not inside a group
 * sub-directory) must not appear in the listing.
 */
TEST(ScenarioUtilityTests, ListScenariosIgnoresNonJsonFilesAndTopLevelFiles)
{
    const auto tempDir = makeEmptyTempDirectory("list-scenarios-ignore");
    const auto executableDirectory = tempDir / "bin";
    filesystem::create_directories(executableDirectory);
    const auto scenarioDirectory = tempDir / "share" / "xmq";
    writeFile(scenarioDirectory / "Basic" / "Connections.json");
    writeFile(scenarioDirectory / "Basic" / "README.txt");
    writeFile(scenarioDirectory / "top-level.json");

    const auto groups = Scenario::listScenarios(executableDirectory);

    ASSERT_EQ(groups.size(), 1u);
    const auto basic = groups.find("Basic");
    ASSERT_NE(basic, groups.end());
    ASSERT_EQ(basic->second.size(), 1u);
    EXPECT_EQ(filesystem::path(basic->second[0]), filesystem::path("Basic") / "Connections.json");

    filesystem::remove_all(tempDir);
}

/**
 * When the installed scenario directory does not exist at all (e.g. running from a build
 * tree rather than an install), listScenarios() returns an empty map rather than throwing.
 */
TEST(ScenarioUtilityTests, ListScenariosReturnsEmptyWhenScenarioDirectoryIsMissing)
{
    const auto tempDir = makeEmptyTempDirectory("list-scenarios-missing");
    const auto executableDirectory = tempDir / "bin";
    filesystem::create_directories(executableDirectory);

    const auto groups = Scenario::listScenarios(executableDirectory);

    EXPECT_TRUE(groups.empty());

    filesystem::remove_all(tempDir);
}

namespace {

// Writes a scenario file, optionally giving the subscriber group a broker of its own.
filesystem::path writeScenarioFile(const filesystem::path& path, const string& subscriberServer)
{
    filesystem::create_directories(path.parent_path());
    ofstream file(path);
    file << R"({
  "name": "group-server-test",
  "type": "Point-To-Point",
  "publishers": {
    "id_prefix": "publisher-",
    "protocol_version": 5,
    "client_count": 1,
    "qos": 1,
    "clean_session": true,
    "topics": "test/topic"
  },
  "subscribers": {
    "id_prefix": "subscriber-",
    "protocol_version": 5,
    "client_count": 1,
    "qos": 1,
    "clean_session": true,
    "topics": "test/topic")"
        << subscriberServer << R"(
  },
  "server": {
    "hostname": "publisher-host",
    "port": 1883,
    "username": "user",
    "password": "secret"
  },
  "parameters": {
    "duration_sec": 1,
    "payload_size": 16,
    "publish_rate": 1
  }
})";
    return path;
}

} // namespace

/**
 * A client group may name a broker of its own, which is what lets a scenario publish to one
 * server and subscribe on another to exercise a bridge.
 */
TEST(ScenarioUtilityTests, ClientGroupCarriesItsOwnServer)
{
    const auto tempDir = makeEmptyTempDirectory("group-server");
    const auto scenarioFile = writeScenarioFile(tempDir / "bridged.json",
                                                R"(,
    "server": {
      "hostname": "subscriber-host",
      "port": 1884,
      "username": "other",
      "password": "other-secret"
    })");

    ScenarioEngine engine;
    engine.load(scenarioFile);
    const auto& scenario = engine.scenario();

    EXPECT_EQ(scenario.m_server.m_hostname.asString(), "publisher-host");
    EXPECT_EQ(scenario.m_server.m_port.asInteger(), 1883);
    EXPECT_EQ(scenario.m_subscribers.m_server.m_hostname.asString(), "subscriber-host");
    EXPECT_EQ(scenario.m_subscribers.m_server.m_port.asInteger(), 1884);
    EXPECT_EQ(scenario.m_subscribers.m_server.m_username.asString(), "other");

    filesystem::remove_all(tempDir);
}

/**
 * The group server is optional: a scenario written without one leaves it empty, and the
 * engine then sends that group to the scenario's own server.
 */
TEST(ScenarioUtilityTests, ClientGroupWithoutServerLeavesItEmpty)
{
    const auto tempDir = makeEmptyTempDirectory("group-server-absent");
    const auto scenarioFile = writeScenarioFile(tempDir / "plain.json", "");

    ScenarioEngine engine;
    engine.load(scenarioFile);
    const auto& scenario = engine.scenario();

    EXPECT_EQ(scenario.m_server.m_hostname.asString(), "publisher-host");
    EXPECT_TRUE(scenario.m_subscribers.m_server.m_hostname.asString().empty());

    filesystem::remove_all(tempDir);
}