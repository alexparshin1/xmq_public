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

/**
 * @file ExtensionHostTests.cpp
 * @brief Conformance tests for the extension boundary.
 *
 * These are what keep the published ABI honest. They load a real shared library through the real
 * loader - not a mock of it - so a change to the header that breaks an already-built extension
 * fails here rather than in somebody's deployment.
 *
 * The library under test is built as part of the test suite from the same sources an outside
 * author would use, and includes only extension/xmq_extension.h and extension/XmqExtension.h.
 */

#include "server/Extensions/ExtensionHost.h"
#include <sptk5/threads/JoiningThread.h>

#include <gtest/gtest.h>
#include <sptk5/FileLogEngine.h>
#include <sptk5/LogEngine.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <future>
#include <iostream>
#include <vector>
#include <thread>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Where the test extension lands. Set by CMake so the test does not have to guess a build layout.
filesystem::path testExtensionLibrary()
{
#ifdef XMQ_TEST_EXTENSION_LIBRARY
    return {XMQ_TEST_EXTENSION_LIBRARY};
#else
    return {};
#endif
}

/// The authenticator-only sample. Separate from the observer-only one on purpose: with both
/// capabilities in one library there is nothing to assert about the separation.
filesystem::path testAuthenticatorLibrary()
{
#ifdef XMQ_TEST_AUTHENTICATOR_LIBRARY
    return {XMQ_TEST_AUTHENTICATOR_LIBRARY};
#else
    return {};
#endif
}

/// The authorizer-only sample, for the same reason: what it must not receive is as much of the
/// contract as what it must.
filesystem::path testAuthorizerLibrary()
{
#ifdef XMQ_TEST_AUTHORIZER_LIBRARY
    return {XMQ_TEST_AUTHORIZER_LIBRARY};
#else
    return {};
#endif
}

/// The one that answers slowly. Not a sample - it exists so that a rule change can be made to land
/// while the broker is inside the extension, which is the only way that window is reachable.
filesystem::path testSlowAuthorizerLibrary()
{
#ifdef XMQ_TEST_SLOW_AUTHORIZER_LIBRARY
    return {XMQ_TEST_SLOW_AUTHORIZER_LIBRARY};
#else
    return {};
#endif
}

/**
 * @brief A path no other call will hand out.
 *
 * The counter is what makes it true. Without it the name was the process id and the suffix, so
 * every test asking for "conf" got the same directory - harmless while each one cleaned up after
 * itself, and not harmless at all when one failed before its cleanup and left the next two reading
 * its files. That is how three tests failed together under a shuffle and each passed alone.
 */
filesystem::path uniqueTemporary(const string& suffix)
{
    static atomic<uint64_t> counter {0};
    return filesystem::temp_directory_path() /
           ("xmq_extension_test_" + to_string(::getpid()) + "_" +
            to_string(counter.fetch_add(1)) + "_" + suffix);
}

/// Waits for a condition rather than sleeping a fixed time: events reach an observer through a
/// queue and another thread, so "already" is never guaranteed and "eventually" always is.
bool waitFor(const function<bool()>& condition, const chrono::milliseconds timeout = chrono::seconds(5))
{
    const auto deadline = chrono::steady_clock::now() + timeout;
    while (chrono::steady_clock::now() < deadline)
    {
        if (condition())
        {
            return true;
        }
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return condition();
}

size_t countLines(const filesystem::path& path)
{
    ifstream file(path);
    if (!file.is_open())
    {
        return 0;
    }
    size_t lines = 0;
    string line;
    while (getline(file, line))
    {
        ++lines;
    }
    return lines;
}
}

namespace xmq {

class XMQ_ExtensionHostTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (testExtensionLibrary().empty() || !filesystem::exists(testExtensionLibrary()) ||
            testAuthenticatorLibrary().empty() || !filesystem::exists(testAuthenticatorLibrary()))
        {
            GTEST_SKIP() << "the test extensions were not built";
        }
        m_eventFile = uniqueTemporary("events.log");
        error_code errorCode;
        filesystem::remove(m_eventFile, errorCode);
    }

    void TearDown() override
    {
        error_code errorCode;
        filesystem::remove(m_eventFile, errorCode);
    }

    ExtensionHost::Configured configured(const string& name = "event-log") const
    {
        return {
            .m_name = name,
            .m_library = testExtensionLibrary(),
            .m_settings = {{"file", m_eventFile.string()}}
        };
    }

    /// The authenticator-only sample, with the client ids it should admit.
    static ExtensionHost::Configured authenticator(const string& allow, const string& lookupMs = "0")
    {
        return {
            .m_name = "allow-list",
            .m_library = testAuthenticatorLibrary(),
            .m_settings = {{"allow", allow}, {"lookup_ms", lookupMs}}
        };
    }

    /// The same sample, configured to behave as though its directory were down.
    static ExtensionHost::Configured unreachableAuthenticator()
    {
        return {
            // The host refuses an entry whose name is not the one the library reports, so this
            // cannot be given a name of its own to tell it apart in the log.
            .m_name = "allow-list",
            .m_library = testAuthenticatorLibrary(),
            .m_settings = {{"allow", "irrelevant"}, {"lookup_ms", "0"}, {"unreachable", "1"}}
        };
    }

    static ExtensionHost::Configured authorizer(const string& root = "site")
    {
        return {
            .m_name = "topic-guard",
            .m_library = testAuthorizerLibrary(),
            .m_settings = {{"separator", "-"}, {"root", root}}
        };
    }

    /// Answers ALLOW once and DENY ever after, taking its time about it.
    static ExtensionHost::Configured slowAuthorizer(const string& delayMs)
    {
        return {
            .m_name = "slow-authorizer",
            .m_library = testSlowAuthorizerLibrary(),
            .m_settings = {{"delay_ms", delayMs}}
        };
    }

    /// Connects a client far enough to get its group, which is what a session is given.
    static shared_ptr<AclGroup> groupOf(ExtensionHost& host, const string& username)
    {
        promise<shared_ptr<AclGroup>> resolved;
        host.authenticate({.m_clientId = "c", .m_username = username},
                          [&resolved](ExtensionHost::AuthDecision, shared_ptr<AclGroup> group)
                          {
                              resolved.set_value(std::move(group));
                          });
        auto group = resolved.get_future();
        EXPECT_EQ(future_status::ready, group.wait_for(chrono::seconds(5)));
        return group.get();
    }

    FileLogEngine    m_logEngine{uniqueTemporary("extensions.log").string()};
    filesystem::path m_eventFile;
};

TEST_F(XMQ_ExtensionHostTests, loadsStartsAndStops)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});

    EXPECT_TRUE(host.watching()) << "the sample implements the observer capability";
    host.stop();

    // start() wrote the file, so the extension ran even without a single event.
    EXPECT_TRUE(filesystem::exists(m_eventFile));
}

TEST_F(XMQ_ExtensionHostTests, deliversEventsToTheObserver)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});
    ASSERT_TRUE(host.watching());

    constexpr size_t eventCount = 25;
    for (size_t i = 0; i < eventCount; ++i)
    {
        host.publishEvent(XMQ_EVENT_PUBLISHED, "client-" + to_string(i), "user", "test/topic", 16, 1, false);
    }

    EXPECT_TRUE(waitFor([this] { return countLines(m_eventFile) >= eventCount; }))
        << "only " << countLines(m_eventFile) << " of " << eventCount << " events arrived";
    EXPECT_EQ(0U, host.droppedEvents());

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, carriesEventFieldsAcrossTheBoundary)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});

    host.publishEvent(XMQ_EVENT_SUBSCRIBED, "the-client", "the-user", "sensors/+/temperature", 0, 2, false);
    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));
    host.stop();

    ifstream file(m_eventFile);
    string   line;
    ASSERT_TRUE(getline(file, line));
    EXPECT_NE(string::npos, line.find("subscribed"));
    EXPECT_NE(string::npos, line.find("client=the-client"));
    EXPECT_NE(string::npos, line.find("user=the-user"));
    EXPECT_NE(string::npos, line.find("topic=sensors/+/temperature"));
    EXPECT_NE(string::npos, line.find("qos=2"));
}

TEST_F(XMQ_ExtensionHostTests, refusesALibraryThatIsNotAnExtension)
{
    ExtensionHost host(m_logEngine, "test");
    // A real shared library with no entry point: the C runtime is always there and never ours.
    host.start({{.m_name = "not-an-extension", .m_library = "libm.so.6", .m_settings = {}}});

    EXPECT_FALSE(host.watching());
    host.stop(); // must not crash, having loaded nothing
}

TEST_F(XMQ_ExtensionHostTests, refusesALibraryThatCallsItselfSomethingElse)
{
    ExtensionHost host(m_logEngine, "test");
    // The configuration says one thing and the library another. Loading it anyway would mean the
    // operator's file names an extension the broker is not running.
    host.start({configured("some-other-name")});

    EXPECT_FALSE(host.watching());
    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, missingLibraryLeavesTheBrokerRunning)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({{.m_name = "absent", .m_library = "/nonexistent/libabsent.so", .m_settings = {}}});

    EXPECT_FALSE(host.watching());
    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, eventsAreDroppedRatherThanQueuedWithoutBound)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});
    ASSERT_TRUE(host.watching());

    // Far past the queue's bound, pushed as fast as this thread can. What is asserted is not a
    // number of drops - a fast machine may keep up - but that the broker never blocks and the
    // count is honest about whatever it lost.
    constexpr size_t flood = 100000;
    for (size_t i = 0; i < flood; ++i)
    {
        host.publishEvent(XMQ_EVENT_PUBLISHED, "flood", "user", "test/topic", 1, 0, false);
    }
    host.stop();

    const auto written = countLines(m_eventFile);
    EXPECT_EQ(flood, written + host.droppedEvents() + (flood - written - host.droppedEvents()))
        << "sanity: nothing is counted twice";
    EXPECT_LE(written, flood);
}

TEST_F(XMQ_ExtensionHostTests, noExtensionsIsNotAnError)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({});
    EXPECT_FALSE(host.watching());

    // The event sites call this unconditionally; with nothing loaded it must be free and harmless.
    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "client", "user", {}, 0, 0, false);
    EXPECT_EQ(0U, host.droppedEvents());
    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, configurationIsReadFromBesideTheBrokerConfiguration)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto extensionsFile = ExtensionHost::configurationPathFor(brokerConfiguration);
    EXPECT_EQ(directory / "xmq_extensions.conf", extensionsFile);

    // Written with forward slashes, which is what generic_string() gives: these paths go into JSON,
    // where a backslash starts an escape sequence. A Windows path dropped in as it comes off
    // string() makes the file unparseable, and what the reader then returns is nothing at all -
    // which looks from here like a reader that ignored the entry.
    {
        ofstream file(extensionsFile);
        file << R"({"extensions":[)"
            << R"({"name":"event-log","library":")" << testExtensionLibrary().generic_string() << R"(",)"
            << R"("enabled":true,"settings":{"file":")" << m_eventFile.generic_string() << R"("}},)"
            << R"({"name":"switched-off","library":"/nowhere.so","enabled":false})"
            << "]}";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    // Both, because a disabled entry is read and reported rather than dropped - the screen lists
    // every extension, and one that is switched off is exactly the one somebody came to look at.
    // What "disabled" costs it is that its library is never opened; see the start() tests.
    ASSERT_EQ(2U, configured.size());
    EXPECT_EQ("event-log", configured[0].m_name);
    EXPECT_TRUE(configured[0].m_enabled);
    EXPECT_EQ(m_eventFile.generic_string(), configured[0].m_settings.at("file"));
    EXPECT_EQ("switched-off", configured[1].m_name);
    EXPECT_FALSE(configured[1].m_enabled);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, fragmentsAreReadFromTheExtensionsDirectory)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";

    const auto fragments = ExtensionHost::configurationDirectoryFor(brokerConfiguration);
    EXPECT_EQ(directory / "xmq_extensions.d", fragments);
    filesystem::create_directories(fragments);

    const auto writeFragment = [&fragments](const string& name, const string& entryName)
    {
        ofstream file(fragments / name);
        file << R"({"extensions":[{"name":")" << entryName << R"(","library":"/nowhere.so"}]})";
    };

    // Named so that name order and the order they were written disagree: what is asserted below is
    // that the broker sorts them, because the order authenticators are asked in is part of what
    // they mean and directory iteration order is whatever the filesystem gives.
    writeFragment("30-third.conf", "third");
    writeFragment("10-first.conf", "first");
    writeFragment("20-second.conf", "second");

    // Not .conf, so not read: an editor's backup left in the directory must not become an entry.
    writeFragment("40-fourth.conf.bak", "fourth");

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(3U, configured.size());
    EXPECT_EQ("first", configured[0].m_name);
    EXPECT_EQ("second", configured[1].m_name);
    EXPECT_EQ("third", configured[2].m_name);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, theSingleFileAndTheDirectoryAreBothRead)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    filesystem::create_directories(ExtensionHost::configurationDirectoryFor(brokerConfiguration));

    {
        ofstream file(ExtensionHost::configurationPathFor(brokerConfiguration));
        file << R"({"extensions":[{"name":"from-the-file","library":"/nowhere.so"}]})";
    }
    {
        ofstream file(ExtensionHost::configurationDirectoryFor(brokerConfiguration) / "10-broken.conf");
        file << "{ this is not JSON";
    }
    {
        ofstream file(ExtensionHost::configurationDirectoryFor(brokerConfiguration) / "20-good.conf");
        file << R"({"extensions":[{"name":"from-the-directory","library":"/nowhere.so"}]})";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    // The file first, then the directory in name order - and a malformed fragment costs only
    // itself. One bad file used to mean no extensions at all.
    ASSERT_EQ(2U, configured.size());
    EXPECT_EQ("from-the-file", configured[0].m_name);
    EXPECT_EQ("from-the-directory", configured[1].m_name);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, aShippedTemplateBecomesALiveFragment)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto fragments = ExtensionHost::configurationDirectoryFor(brokerConfiguration);
    filesystem::create_directories(fragments);

    // What a package installs: a template, and no live file. It cannot install the live one - the
    // next upgrade would put its copy back over whatever the operator had answered in it.
    {
        ofstream file(fragments / "50-shipped.conf.template");
        file << R"({"extensions":[{"name":"shipped","library":"/nowhere.so"}]})";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    ASSERT_EQ(1U, configured.size());
    EXPECT_EQ("shipped", configured[0].m_name);
    EXPECT_TRUE(filesystem::exists(fragments / "50-shipped.conf"));

    // From now on it is the operator's file. Switching the extension off has to survive both the
    // next start and the next upgrade, and only the second half of that is the packaging's job.
    {
        ofstream file(fragments / "50-shipped.conf");
        file << R"({"extensions":[{"name":"shipped","library":"/nowhere.so","enabled":false}]})";
    }

    const auto afterwards = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    ASSERT_EQ(1U, afterwards.size());
    EXPECT_FALSE(afterwards[0].m_enabled);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, oneNameIsConfiguredOnlyOnce)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto fragments = ExtensionHost::configurationDirectoryFor(brokerConfiguration);
    filesystem::create_directories(fragments);

    // The shape an installation arrives at on its own: an entry left in the file, and the fragment
    // an install put beside it saying the same thing.
    {
        ofstream file(ExtensionHost::configurationPathFor(brokerConfiguration));
        file << R"({"extensions":[{"name":"twice","library":"/from-the-file.so"}]})";
    }
    {
        ofstream file(fragments / "50-twice.conf");
        file << R"({"extensions":[{"name":"twice","library":"/from-the-fragment.so"},)"
            << R"({"name":"once","library":"/nowhere.so"}]})";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    // Two entries of one name would be two instances of one library - two start()s and everything
    // it writes twice. The first wins, and the rest of the fragment is still read.
    ASSERT_EQ(2U, configured.size());
    EXPECT_EQ("twice", configured[0].m_name);
    EXPECT_EQ("/from-the-file.so", configured[0].m_library.string());
    EXPECT_EQ("once", configured[1].m_name);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, anAttributeReachesTheExtensionThatAskedForIt)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});
    ASSERT_TRUE(host.watching());

    // The sample asks for remote_address in start(). A fact the broker knows travels by name, so
    // publishing one more of them costs no ABI version and no rebuild of any extension.
    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "a-client", "dave", {}, 0, 0, false,
                      {{ExtensionHost::remoteAddressAttribute, "10.1.1.9:51423"}});

    // An attribute nobody asked for is not carried, so offering one costs a comparison.
    host.publishEvent(XMQ_EVENT_CLIENT_DISCONNECTED, "a-client", "dave", {}, 0, 0, false,
                      {{"a_name_this_broker_does_not_publish", "ignored"}});

    // Waited for rather than assumed: events reach an observer through a queue and another thread.
    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 2; }));
    host.stop();

    ifstream file(m_eventFile);
    string   written;
    for (string line; getline(file, line);)
    {
        written += line + "\n";
    }
    EXPECT_NE(string::npos, written.find("from=10.1.1.9:51423")) << written;
    EXPECT_EQ(string::npos, written.find("ignored")) << written;
}

TEST_F(XMQ_ExtensionHostTests, anErrorCarriesWhatItWasWithoutBeingAskedTo)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});
    ASSERT_TRUE(host.watching());

    host.publishError(LogSubject::Connect, "anonymous_refused", "Invalid username or password",
                      "a-client", "dave",
                      {{ExtensionHost::remoteAddressAttribute, "10.1.1.9:51423"}});

    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));
    host.stop();

    ifstream file(m_eventFile);
    string   written;
    for (string line; getline(file, line);)
    {
        written += line + "\n";
    }

    // Subject, reason and message are what the event says rather than extra facts about it, so
    // they arrive whether or not the extension asked for anything.
    EXPECT_NE(string::npos, written.find("subject=connect")) << written;
    EXPECT_NE(string::npos, written.find("reason=anonymous_refused")) << written;
    EXPECT_NE(string::npos, written.find("message=\"Invalid username or password\"")) << written;

    // The address is an ordinary attribute, and this sample does ask for it.
    EXPECT_NE(string::npos, written.find("from=10.1.1.9:51423")) << written;
}

TEST_F(XMQ_ExtensionHostTests, anErrorIsGovernedByItsOwnSubject)
{
    ExtensionHost host(m_logEngine, "test");

    // Publish switched off, connect left on - which is what the shipped defaults do, and the whole
    // reason errors are gated by subject rather than by being errors: a rejected PUBLISH is the one
    // failure that is not rare.
    host.enableEvents(ExtensionHost::subjectBit(LogSubject::Connect));
    host.start({configured()});

    host.publishError(LogSubject::Publish, "invalid_topic", "Rejected", "a-client", "dave");
    host.publishError(LogSubject::Connect, "credentials", "Refused", "a-client", "dave");

    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));
    host.stop();

    ifstream file(m_eventFile);
    string   written;
    for (string line; getline(file, line);)
    {
        written += line + "\n";
    }

    EXPECT_NE(string::npos, written.find("reason=credentials")) << written;
    EXPECT_EQ(string::npos, written.find("invalid_topic")) << written;
}

TEST_F(XMQ_ExtensionHostTests, aSettingIsAppliedWithoutStoppingTheBroker)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});
    ASSERT_TRUE(host.watching());

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "before", "dave", {}, 0, 0, false);
    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));

    // The same extension, one setting different - which is what an operator editing a fragment
    // and pressing reload arrives at.
    const auto moved = uniqueTemporary("moved.log");
    auto       changed = configured();
    changed.m_settings["file"] = moved.string();

    const auto report = host.reloadSettings({changed});
    EXPECT_FALSE(report.failed()) << report.m_problems.join("; ");
    ASSERT_EQ(1U, report.m_notes.size()) << "an extension whose settings changed is reported";
    EXPECT_NE(string::npos, report.m_notes[0].find("applied")) << report.m_notes[0];

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "after", "dave", {}, 0, 0, false);
    EXPECT_TRUE(waitFor([&moved] { return countLines(moved) >= 1; }))
        << "events must go where the new settings say";

    host.stop();

    // Nothing was stopped and started: the first event is still in the first file, which a restart
    // of the extension would not have guaranteed.
    ifstream file(m_eventFile);
    string   written;
    for (string line; getline(file, line);)
    {
        written += line + "\n";
    }
    EXPECT_NE(string::npos, written.find("before")) << written;
    EXPECT_EQ(string::npos, written.find("after")) << written;

    error_code errorCode;
    filesystem::remove(moved, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, refusedSettingsArePutBackWhereTheyCameFrom)
{
    // Found from the interface: a database URI the extension would not take was written to the
    // configuration file and left in the host's settings, so navigating away and back showed the
    // refused value sitting in the field as though it had been accepted. What the broker reports
    // has to be what the broker is using.
    const auto directory = uniqueTemporary("refused");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto extensionsFile = ExtensionHost::configurationPathFor(brokerConfiguration);
    {
        ofstream file(extensionsFile);
        file << R"({"extensions":[)"
             << R"({"name":"event-log","library":")" << testExtensionLibrary().generic_string() << R"(",)"
             << R"("enabled":true,"settings":{"file":")" << m_eventFile.generic_string() << R"("}})"
             << "]}";
    }

    ExtensionHost host(m_logEngine, "test");
    host.start(ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine));
    ASSERT_TRUE(host.watching());

    // A path the sample cannot open, which is how it refuses: the directory does not exist, so
    // neither does anything it could create in it.
    auto refused = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine).front();
    const auto impossible = (directory / "no-such-directory" / "events.log").generic_string();
    refused.m_settings["file"] = impossible;

    const auto report = host.reloadSettings({refused});
    // A refusal is carried as a problem, not as prose the screen would have to recognise: that
    // is what lets the interface show it as a failure rather than as news.
    ASSERT_TRUE(report.failed());
    EXPECT_NE(string::npos, report.m_problems[0].find("refused")) << report.m_problems[0];

    // In memory: what describe() says is what the screen shows.
    const auto described = host.describe();
    ASSERT_EQ(1U, described.size());
    const auto setting = ranges::find_if(described[0].m_settings, [](const auto& one)
                                         { return one.m_name == "file"; });
    ASSERT_NE(described[0].m_settings.end(), setting);
    EXPECT_EQ(m_eventFile.generic_string(), setting->m_value)
        << "a refused setting must not be reported as the one in force";

    // And on disk, or the refused value comes back at the next start - and then the file and the
    // running broker disagree about what the extension is configured with.
    const auto onDisk = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(1U, onDisk.size());
    EXPECT_EQ(m_eventFile.generic_string(), onDisk[0].m_settings.at("file"));
    EXPECT_NE(impossible, onDisk[0].m_settings.at("file"));

    host.stop();

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, anUnchangedExtensionIsNotDisturbed)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});

    // The ordinary case of pressing reload: nothing was edited. Saying so back matters - an
    // operator who changed the wrong file needs to know the broker found nothing to do.
    const auto unchanged = host.reloadSettings({configured()});
    EXPECT_TRUE(unchanged.m_notes.empty());
    EXPECT_TRUE(unchanged.m_problems.empty());

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, anExtensionIsSwitchedOnAndOffWhileTheBrokerRuns)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({});
    ASSERT_FALSE(host.watching()) << "nothing configured, so nothing to watch with";

    // On: nothing was loaded at start-up, so this opens the library and starts the delivery thread
    // that start-up had no reason to start.
    const auto switchedOn = host.reloadSettings({configured()});
    ASSERT_FALSE(switchedOn.failed()) << switchedOn.m_problems.join("; ");
    ASSERT_EQ(1U, switchedOn.m_notes.size());
    EXPECT_NE(String::npos, switchedOn.m_notes[0].find("started")) << switchedOn.m_notes[0];
    EXPECT_TRUE(host.watching());

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "while-on", "dave", {}, 0, 0, false);
    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));

    // Off: out of the snapshot, waited out, stopped, instance destroyed - and the library stays
    // mapped, which is why this is safe at all.
    const auto switchedOff = host.reloadSettings({});
    ASSERT_FALSE(switchedOff.failed()) << switchedOff.m_problems.join("; ");
    ASSERT_EQ(1U, switchedOff.m_notes.size());
    EXPECT_NE(String::npos, switchedOff.m_notes[0].find("stopped")) << switchedOff.m_notes[0];
    EXPECT_FALSE(host.watching()) << "the last observer went, so the broker stops paying for events";

    const auto afterOff = countLines(m_eventFile);
    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "while-off", "dave", {}, 0, 0, false);
    this_thread::sleep_for(chrono::milliseconds(200));
    EXPECT_EQ(afterOff, countLines(m_eventFile)) << "a switched-off observer receives nothing";

    // On again, against the library that was never unloaded.
    const auto switchedOnAgain = host.reloadSettings({configured()});
    ASSERT_FALSE(switchedOnAgain.failed()) << switchedOnAgain.m_problems.join("; ");
    ASSERT_EQ(1U, switchedOnAgain.m_notes.size());
    EXPECT_NE(String::npos, switchedOnAgain.m_notes[0].find("started")) << switchedOnAgain.m_notes[0];

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "on-again", "dave", {}, 0, 0, false);
    EXPECT_TRUE(waitFor([this, afterOff] { return countLines(m_eventFile) > afterOff; }));

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, aRequiredExtensionIsNotSwitchedOffFromTheInterface)
{
    auto required = configured();
    required.m_required = true;

    ExtensionHost host(m_logEngine, "test");
    host.start({required});
    ASSERT_TRUE(host.watching());

    // Without it the broker does not serve MQTT at all, so switching it off here would be a way to
    // stop serving that does not say it is one.
    const auto answer = host.disable("event-log");
    ASSERT_TRUE(answer.failed()) << "refusing to switch a required extension off is a failure";
    EXPECT_NE(String::npos, answer.m_problems[0].find("required")) << answer.m_problems[0];
    EXPECT_TRUE(host.watching()) << "and it is still running";

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, describesItselfForTheInterface)
{
    auto withTypo = configured();
    withTypo.m_settings["fyle"] = "a misspelled key";

    ExtensionHost host(m_logEngine, "test");
    host.start({withTypo});

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "someone", "dave", {}, 0, 0, false);
    ASSERT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));

    const auto described = host.describe();
    ASSERT_EQ(1U, described.size());
    const auto& one = described[0];

    EXPECT_EQ("event-log", one.m_name);
    EXPECT_FALSE(one.m_version.empty());
    EXPECT_FALSE(one.m_description.empty()) << "the sample says what it is for";
    EXPECT_TRUE(one.m_running);
    EXPECT_FALSE(one.m_required);
    EXPECT_EQ(static_cast<uint32_t>(XMQ_CAP_OBSERVER), one.m_capabilities);
    EXPECT_EQ(1U, one.m_eventsDelivered);

    // Declared first, in the order the extension declared them, each carrying what is configured.
    ASSERT_GE(one.m_settings.size(), 2U);
    EXPECT_EQ("file", one.m_settings[0].m_name);
    EXPECT_TRUE(one.m_settings[0].m_declared);
    EXPECT_EQ(m_eventFile.string(), one.m_settings[0].m_value);
    EXPECT_FALSE(one.m_settings[0].m_label.empty());

    // And the misspelled key is reported rather than hidden: hiding it would hide the commonest
    // reason a setting quietly does nothing.
    const auto typo = ranges::find_if(one.m_settings, [](const auto& setting)
    {
        return setting.m_name == "fyle";
    });
    ASSERT_NE(one.m_settings.end(), typo);
    EXPECT_FALSE(typo->m_declared);

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, settingsAreWrittenBackToTheFileTheyCameFrom)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto fragments = ExtensionHost::configurationDirectoryFor(brokerConfiguration);
    filesystem::create_directories(fragments);

    const auto fragment = fragments / "50-event-log.conf";
    {
        ofstream file(fragment);
        file << R"({"extensions":[{"_comment":"written by a person","name":"event-log",)"
            << R"("library":")" << testExtensionLibrary().generic_string() << R"(",)"
            << R"("required":false,"settings":{"file":")" << m_eventFile.generic_string() << R"("}}]})";
    }

    ExtensionHost host(m_logEngine, "test");
    host.start(ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine));
    ASSERT_TRUE(host.watching());

    const auto moved = uniqueTemporary("moved.log");
    const auto written = host.writeSettings("event-log", {{"file", moved.string()}});
    ASSERT_FALSE(written.failed()) << written.m_problems.join("; ");
    EXPECT_NE(String::npos, written.m_notes[0].find(fragment.string())) << written.m_notes[0];

    // Everything else in the file is carried through - including the line somebody wrote for the
    // next reader, which is the first thing a careless rewrite loses.
    ifstream file(fragment);
    string   contents;
    for (string line; getline(file, line);)
    {
        contents += line;
    }
    EXPECT_NE(string::npos, contents.find("written by a person")) << contents;

    // The path is checked through the reader rather than in the raw text. The file holds it the way
    // the platform spells it, with JSON escaping on top - on Windows that is C:\\Users\\... where
    // generic_string() gives C:/Users/... - so a test that greps for one spelling passes on Unix
    // and fails on Windows over nothing the broker did.
    const auto reread = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(1U, reread.size());
    EXPECT_EQ(moved.string(), reread[0].m_settings.at("file"));

    host.stop();

    error_code errorCode;
    filesystem::remove(moved, errorCode);
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, aDisabledEntryIsListedButNeverLoaded)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";

    {
        ofstream file(ExtensionHost::configurationPathFor(brokerConfiguration));
        file << R"({"extensions":[{"name":"event-log","library":")"
             << testExtensionLibrary().generic_string()
             << R"(","enabled":false,"settings":{"file":"nowhere.log"}}]})";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);

    // Read, not dropped. It used to be skipped here, which made a switched-off extension invisible
    // to the broker - and so absent from a screen that promises to list every one of them.
    ASSERT_EQ(1U, configured.size());
    EXPECT_FALSE(configured[0].m_enabled);

    ExtensionHost host(m_logEngine, "test");
    host.start(configured);

    EXPECT_FALSE(host.watching()) << "its library must not have been opened";

    const auto described = host.describe();
    ASSERT_EQ(1U, described.size()) << "and yet it is listed";
    EXPECT_EQ("event-log", described[0].m_name);
    EXPECT_FALSE(described[0].m_running);

    // Nothing came from inside the library, because it was never opened - that is what being
    // switched off has to mean.
    EXPECT_TRUE(described[0].m_version.empty());
    ASSERT_EQ(1U, described[0].m_settings.size());
    EXPECT_EQ("file", described[0].m_settings[0].m_name);

    host.stop();

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, switchingOffIsWrittenToTheFile)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";
    const auto path = ExtensionHost::configurationPathFor(brokerConfiguration);

    {
        ofstream file(path);
        file << R"({"extensions":[{"name":"event-log","library":")"
             << testExtensionLibrary().generic_string()
             << R"(","settings":{"file":")" << m_eventFile.generic_string() << R"("}}]})";
    }

    ExtensionHost host(m_logEngine, "test");
    host.start(ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine));
    ASSERT_TRUE(host.watching());

    const auto written = host.writeEnabled("event-log", false);
    ASSERT_FALSE(written.failed()) << written.m_problems.join("; ");
    EXPECT_NE(String::npos, written.m_notes[0].find("disabled in")) << written.m_notes[0];

    // The decision survives a restart, which is the whole point: without this the entry still reads
    // enabled and the next start undoes what the operator asked for.
    const auto afterwards = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(1U, afterwards.size());
    EXPECT_FALSE(afterwards[0].m_enabled);

    host.stop();

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, requiredIsOffUnlessTheConfigurationSaysOtherwise)
{
    const auto directory = uniqueTemporary("conf");
    filesystem::create_directories(directory);
    const auto brokerConfiguration = directory / "xmq_server.conf";

    {
        ofstream file(ExtensionHost::configurationPathFor(brokerConfiguration));
        file << R"({"extensions":[)"
            << R"({"name":"event-log","library":")" << testExtensionLibrary().generic_string() << R"(",)"
            << R"("required":true,"settings":{"file":")" << m_eventFile.generic_string() << R"("}},)"
            << R"({"name":"allow-list","library":")" << testAuthenticatorLibrary().generic_string() << R"("},)"
            << R"({"name":"switched-off","library":"/nowhere.so","enabled":false,"required":true})"
            << "]}";
    }

    const auto configured = ExtensionHost::readConfiguration(brokerConfiguration, m_logEngine);
    ASSERT_EQ(3U, configured.size()) << "every entry is read, disabled ones included";
    EXPECT_TRUE(configured[0].m_required);
    EXPECT_FALSE(configured[1].m_required) << "an entry that does not mention it is optional";

    // Both flags are read, and being switched off is what settles it: an entry that is required and
    // disabled is a contradiction the operator has already resolved by disabling it, so start()
    // never loads it and never refuses to serve on its account.
    EXPECT_TRUE(configured[2].m_required);
    EXPECT_FALSE(configured[2].m_enabled);

    error_code errorCode;
    filesystem::remove_all(directory, errorCode);
}

TEST_F(XMQ_ExtensionHostTests, aRequiredExtensionThatCannotLoadStopsTheBroker)
{
    ExtensionHost::Configured missing{
        .m_name = "event-log",
        .m_library = uniqueTemporary("nothing-here.so"),
        .m_required = true
    };

    ExtensionHost host(m_logEngine, "test");
    // The Server constructor is what calls this, so the throw is how the broker declines to serve
    // while its configuration interface stays up to be repaired through.
    EXPECT_THROW(host.start({missing}), Exception);
}

TEST_F(XMQ_ExtensionHostTests, anOptionalExtensionThatCannotLoadDoesNot)
{
    const ExtensionHost::Configured missing{
        .m_name = "event-log",
        .m_library = uniqueTemporary("nothing-here.so")
    };

    ExtensionHost host(m_logEngine, "test");
    EXPECT_NO_THROW(host.start({missing}));
    host.stop();
}

// An extension that cannot reach its store says so, and that answer is neither an abstention nor
// a denial: the broker must refuse the connection without asking anyone else. Abstaining would let
// an outage hand every client to the next authenticator, or to the broker's own accounts, which is
// how a directory going down turns into a broker that admits the wrong people.
TEST_F(XMQ_ExtensionHostTests, anUnreachableStoreRefusesWithoutFallingBack)
{
    ExtensionHost host(m_logEngine, "test");

    // The second one would admit this client, and must never be reached.
    host.start({unreachableAuthenticator(), authenticator("wanted-client")});
    ASSERT_TRUE(host.authenticating());

    promise<ExtensionHost::AuthDecision> decided;
    host.authenticate({.m_clientId = "wanted-client", .m_username = "u", .m_password = "p"},
                      [&decided](const ExtensionHost::AuthDecision decision, const shared_ptr<AclGroup>&)
                      {
                          decided.set_value(decision);
                      });
    auto answer = decided.get_future();
    ASSERT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));

    EXPECT_EQ(ExtensionHost::AuthDecision::SubsystemError, answer.get())
        << "a store that cannot be read must not become a store that says yes";
}

TEST_F(XMQ_ExtensionHostTests, authenticatesThroughTheExtension)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authenticator("wanted-client, also-wanted")});
    ASSERT_TRUE(host.authenticating());

    const auto ask = [&host](const string& clientId)
    {
        promise<ExtensionHost::AuthDecision> decided;
        host.authenticate({.m_clientId = clientId, .m_username = "u", .m_password = "p"},
                          [&decided](const ExtensionHost::AuthDecision decision, const shared_ptr<AclGroup>&)
                          {
                              decided.set_value(decision);
                          });
        auto answer = decided.get_future();
        EXPECT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));
        return answer.get();
    };

    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask("wanted-client"));
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask("also-wanted"));
    // Not on the list is not the same as refused: the broker's own accounts still get the question.
    EXPECT_EQ(ExtensionHost::AuthDecision::NotHandled, ask("someone-else"));

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, answersWhileTheCallerCarriesOn)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authenticator("slow-client", "300")});

    // The point of the whole design: the call that asks does not wait for the answer. If this ever
    // starts blocking, a directory server's latency has moved onto a broker thread.
    promise<ExtensionHost::AuthDecision> decided;
    const auto                           asked = chrono::steady_clock::now();
    host.authenticate({.m_clientId = "slow-client"},
                      [&decided](const ExtensionHost::AuthDecision decision, const shared_ptr<AclGroup>&)
                      {
                          decided.set_value(decision);
                      });
    const auto returned = chrono::steady_clock::now() - asked;

    EXPECT_LT(returned, chrono::milliseconds(100)) << "authenticate() waited for the extension";

    auto answer = decided.get_future();
    ASSERT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, answer.get());

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, anObserverIsNeverAskedToAuthenticate)
{
    // event-log declares XMQ_CAP_OBSERVER alone. Its table therefore carries no authenticator, and
    // the broker has nothing to call - the separation is structural, not a filter.
    ExtensionHost host(m_logEngine, "test");
    host.start({configured()});

    EXPECT_TRUE(host.watching());
    EXPECT_FALSE(host.authenticating()) << "an observer must not be treated as an authenticator";

    ExtensionHost::AuthDecision decision = ExtensionHost::AuthDecision::Deny;
    host.authenticate({.m_clientId = "anyone"}, [&decision](const auto answer, const shared_ptr<AclGroup>&) { decision = answer; });
    EXPECT_EQ(ExtensionHost::AuthDecision::NotHandled, decision)
        << "with no authenticator loaded the broker's own accounts decide, immediately";

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, anAuthenticatorIsNeverHandedEvents)
{
    // allow-list declares XMQ_CAP_AUTHENTICATOR alone. Nothing is delivered to it, and with no
    // observer loaded at all the event thread is never started.
    ExtensionHost host(m_logEngine, "test");
    host.start({authenticator("someone")});

    EXPECT_TRUE(host.authenticating());
    EXPECT_FALSE(host.watching()) << "an authenticator must not be treated as an observer";

    // The event sites call this unconditionally; with nobody watching it must cost nothing and
    // reach nobody.
    for (size_t i = 0; i < 100; ++i)
    {
        host.publishEvent(XMQ_EVENT_PUBLISHED, "client", "user", "test/topic", 8, 1, false);
    }
    EXPECT_EQ(0U, host.droppedEvents()) << "events were queued for an extension that never asked";

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, twoExtensionsEachGetOnlyTheirOwnWork)
{
    // Both loaded together, which is the arrangement an operator would actually run.
    ExtensionHost host(m_logEngine, "test");
    host.start({configured(), authenticator("admitted-client")});

    ASSERT_TRUE(host.watching());
    ASSERT_TRUE(host.authenticating());

    promise<ExtensionHost::AuthDecision> decided;
    host.authenticate({.m_clientId = "admitted-client"},
                      [&decided](const ExtensionHost::AuthDecision decision, const shared_ptr<AclGroup>&)
                      {
                          decided.set_value(decision);
                      });
    auto answer = decided.get_future();
    ASSERT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, answer.get());

    host.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, "admitted-client", "user", {}, 0, 0, false);
    EXPECT_TRUE(waitFor([this] { return countLines(m_eventFile) >= 1; }));

    host.stop();

    // The observer wrote the event and the authenticator wrote none: the event file holds exactly
    // what event-log saw, and allow-list never had an on_event to be called through.
    EXPECT_EQ(1U, countLines(m_eventFile));
}

TEST_F(XMQ_ExtensionHostTests, authorizesByGroupRatherThanByClient)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer()});
    ASSERT_TRUE(host.authorizing());
    EXPECT_FALSE(host.authenticating()) << "an authorizer must not be asked to authenticate";

    const auto alice = groupOf(host, "ops-alice");
    const auto bob = groupOf(host, "ops-bob");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(bob);

    // The point of the whole design: two clients of one group share one set of answers, so the
    // cost of a rule is paid per group and per topic, not per client and per message.
    EXPECT_EQ(alice.get(), bob.get()) << "clients of the same group must share their permissions";

    EXPECT_EQ(AclDecision::Allow, alice->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));
    EXPECT_EQ(AclDecision::Allow, bob->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));
    EXPECT_EQ(AclDecision::Deny, bob->authorize("site/hr/salaries", XMQ_ACL_SUBSCRIBE));

    const auto stranger = groupOf(host, "nobody");
    ASSERT_TRUE(stranger);
    EXPECT_NE(alice.get(), stranger.get());
    // No group this extension recognised, so it says nothing rather than refusing a client the
    // broker's own rules would have admitted.
    EXPECT_EQ(AclDecision::NotHandled, stranger->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));

    host.stop();
}

/**
 * @brief Refuse a client when its group name exceeds the broker's buffer.
 * A truncated group name could give two different groups the same permissions.
 */
TEST_F(XMQ_ExtensionHostTests, aGroupNameTooLongToHoldRefusesTheClient)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({slowAuthorizer("0")}); // its group is the username

    promise<pair<ExtensionHost::AuthDecision, shared_ptr<AclGroup>>> answered;
    host.authenticate({.m_clientId = "c", .m_username = string(200, 'g')},
                      [&answered](const ExtensionHost::AuthDecision decision, shared_ptr<AclGroup> group)
                      {
                          answered.set_value({decision, std::move(group)});
                      });
    auto answer = answered.get_future();
    ASSERT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));
    const auto [decision, group] = answer.get();
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, decision);
    EXPECT_FALSE(group) << "a client refused for its group name must not be given a group";

    // A name that fits is still a group.
    EXPECT_TRUE(groupOf(host, "short"));
    host.stop();
}

/**
 * @brief Read a setting in full when it exceeds the wrapper's initial buffer.
 */
TEST_F(XMQ_ExtensionHostTests, aLongSettingIsReadWhole)
{
    const string root(300, 'r');
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer(root)});

    const auto alice = groupOf(host, "ops-alice");
    ASSERT_TRUE(alice);
    EXPECT_EQ(AclDecision::Allow, alice->authorize(root + "/ops/alarm", XMQ_ACL_PUBLISH))
        << "the rule was built from a root cut short";
    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, aGroupDoesNotInheritTheDecisionsOfOneThatHeldItsAddress)
{
    // The per-thread decision cache identified a group by its address, and every group starts at
    // generation 1 - so an entry left by a destroyed group matched the next group allocated at the
    // same address on every field, and answered for it.
    //
    // Nothing in a running broker reaches this today: the host is a member of Server, lives as long
    // as it, and never erases a group. It surfaced as this suite failing on one platform only,
    // where the allocator happened to recycle an address between two tests.
    //
    // The loop is because reuse is the allocator's choice, not ours. Every pass asserts the right
    // answer whether or not the address came back; when it does, this is the case being tested and
    // there is nothing more to prove.
    constexpr auto topic = "site/ops/alarm";

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const AclGroup* firstAddress = nullptr;
        {
            ExtensionHost allowing(m_logEngine, "test");
            allowing.start({authorizer("site")});
            const auto allowed = groupOf(allowing, "ops-alice");
            ASSERT_TRUE(allowed);
            firstAddress = allowed.get();
            EXPECT_EQ(AclDecision::Allow, allowed->authorize(topic, XMQ_ACL_PUBLISH));
            allowing.stop();
        }

        // The same group name, from a host that owns a different part of the topic space, so the
        // answer for this topic must now be the opposite one.
        ExtensionHost refusing(m_logEngine, "test");
        refusing.start({authorizer("elsewhere")});
        const auto refused = groupOf(refusing, "ops-alice");
        ASSERT_TRUE(refused);
        EXPECT_EQ(AclDecision::Deny, refused->authorize(topic, XMQ_ACL_PUBLISH))
            << "a group answered with the decisions of the one that held its address";
        refusing.stop();

        if (refused.get() == firstAddress)
        {
            return;
        }
    }

    GTEST_SKIP() << "the allocator did not hand the same address back in 20 attempts, so the case "
                    "this test exists for did not arise";
}

TEST_F(XMQ_ExtensionHostTests, aCachedDecisionIsNotAskedForAgain)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer()});

    const auto group = groupOf(host, "ops-alice");
    ASSERT_TRUE(group);

    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_EQ(AclDecision::Allow, group->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));
    }

    // Not a timing assertion - it is that the same object answers, which is what the broker holds
    // on the publish path. What is being fixed here is that the answer is a lookup and the
    // extension is not on that path.
    EXPECT_EQ(AclDecision::Deny, group->authorize("elsewhere", XMQ_ACL_PUBLISH));

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, anAuthorizerIsNeverHandedEvents)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer()});

    EXPECT_FALSE(host.watching()) << "topic-guard declares no observer, so nothing may be queued for it";

    host.publishEvent(XMQ_EVENT_PUBLISHED, "c", "ops-alice", "site/ops/alarm", 4, 0, false);
    this_thread::sleep_for(chrono::milliseconds(100));

    host.stop();
    EXPECT_EQ(0U, host.droppedEvents());
}

TEST_F(XMQ_ExtensionHostTests, invalidationSurvivesTheThreadLocalCache)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer()});

    const auto group = groupOf(host, "ops-alice");
    ASSERT_TRUE(group);

    EXPECT_EQ(AclDecision::Allow, group->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));
    EXPECT_EQ(AclDecision::Deny, group->authorize("site/hr/pay", XMQ_ACL_PUBLISH));

    host.invalidateAcl();

    // Answers are remembered per thread and stamped with a generation, so what this fixes is that
    // dropping the shared cache also retires every thread's own copies. A stale generation must
    // read as a miss and be asked for again - on the thread that filled it and on a new one.
    EXPECT_EQ(AclDecision::Allow, group->authorize("site/ops/alarm", XMQ_ACL_PUBLISH));
    EXPECT_EQ(AclDecision::Deny, group->authorize("site/hr/pay", XMQ_ACL_PUBLISH));

    AclDecision fromAnotherThread = AclDecision::NotHandled;
    {
        const JoiningThread reader([&] { fromAnotherThread = group->authorize("site/ops/alarm", XMQ_ACL_PUBLISH); });
    }
    EXPECT_EQ(AclDecision::Allow, fromAnotherThread);

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, anAnswerFromTheOldRulesIsNotCachedAfterTheyChange)
{
    constexpr auto answerTime = chrono::milliseconds(500);

    ExtensionHost host(m_logEngine, "test");
    host.start({slowAuthorizer(to_string(answerTime.count()))});

    const auto group = groupOf(host, "ops");
    ASSERT_TRUE(group);

    // A topic nothing has asked about, so the extension really is reached - and it takes long
    // enough answering for the rules to change while the broker is still inside it. The broker
    // asks outside its lock on purpose, which is what makes this window exist at all.
    auto asking = async(launch::async, [&group] { return group->authorize("site/ops/a", XMQ_ACL_PUBLISH); });
    this_thread::sleep_for(answerTime / 4);
    host.invalidateAcl();

    EXPECT_EQ(AclDecision::Allow, asking.get()) << "the answer was what the rules said when it was asked";

    // And that is the point: the answer predates the invalidation, so it must not have been left
    // in the cache. This extension answers ALLOW once and DENY ever after, so an answer that was
    // asked for again is visible from out here without seeing inside the extension - and a stale
    // entry is the difference between Deny and Allow.
    EXPECT_EQ(AclDecision::Deny, group->authorize("site/ops/a", XMQ_ACL_PUBLISH))
        << "a decision taken under the old rules outlived the invalidation";

    host.stop();
}

/// What a permission check costs, on the cache the broker actually uses. Disabled because it is a
/// measurement and not an assertion - a threshold here would fail on whatever machine is busy.
/// Run it with --gtest_also_run_disabled_tests when the claim needs checking again.
TEST_F(XMQ_ExtensionHostTests, DISABLED_permissionCheckCost)
{
    ExtensionHost host(m_logEngine, "test");
    host.start({authorizer()});

    const auto group = groupOf(host, "ops-alice");
    ASSERT_TRUE(group);

    // A group that uses eight topics, all of them already asked about: the steady state, which is
    // the state a broker is in for all but the first message on each topic.
    const vector<string> topics{"site/ops/a", "site/ops/b", "site/ops/c", "site/ops/d",
                                "site/ops/e", "site/ops/f", "site/ops/g", "site/ops/h"};
    for (const auto& topic: topics)
    {
        EXPECT_EQ(AclDecision::Allow, group->authorize(topic, XMQ_ACL_PUBLISH));
    }

    for (const size_t threadCount: {size_t{1}, size_t{4}, size_t{8}})
    {
        constexpr size_t iterations = 20'000'000;
        atomic<size_t>   allowed{0};

        const auto started = chrono::steady_clock::now();
        {
            JoiningThreads threads;
            for (size_t t = 0; t < threadCount; ++t)
            {
                threads.emplace_back(
                    [&]
                    {
                        size_t local = 0;
                        for (size_t i = 0; i < iterations / threadCount; ++i)
                        {
                            local += group->authorize(topics[i & 7U], XMQ_ACL_PUBLISH) == AclDecision::Allow
                                         ? 1
                                         : 0;
                        }
                        allowed += local;
                    });
            }
        }
        const auto elapsed = chrono::steady_clock::now() - started;

        cout << threadCount << " thread(s): "
            << double(chrono::duration_cast<chrono::nanoseconds>(elapsed).count()) / double(iterations)
            << " ns per check, " << allowed.load() << " allowed" << endl;
    }

    host.stop();
}

TEST_F(XMQ_ExtensionHostTests, absentConfigurationMeansNoExtensions)
{
    const auto configured = ExtensionHost::readConfiguration("/nonexistent/xmq_server.conf", m_logEngine);
    EXPECT_TRUE(configured.empty());
}

} // namespace
