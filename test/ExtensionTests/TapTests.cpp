#include "server/Extensions/ExtensionHost.h"

#include <gtest/gtest.h>
#include <sptk5/FileLogEngine.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

filesystem::path tapLibrary()
{
#ifdef XMQ_TEST_TAP_RECORDER_LIBRARY
    return {XMQ_TEST_TAP_RECORDER_LIBRARY};
#else
    return {};
#endif
}

class TapTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (tapLibrary().empty() || !filesystem::exists(tapLibrary()))
        {
            GTEST_SKIP() << "the tap-recorder extension was not built";
        }
        m_file = filesystem::temp_directory_path() /
                 ("xmq-tap-" + to_string(::getpid()) + "-" +
                  to_string(chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        filesystem::remove(m_file);
    }

    void TearDown() override
    {
        error_code errorCode;
        filesystem::remove(m_file, errorCode);
    }

    ExtensionHost::Configured extension(const string& filter = "sensors/#",
                                        const string& maxPayload = "0",
                                        const string& delay = "0") const
    {
        return ExtensionHost::Configured {
            .m_name = "tap-recorder",
            .m_library = tapLibrary(),
            .m_settings = {{"path", m_file.string()}, {"filter", filter},
                           {"max_payload", maxPayload}, {"delay_ms", delay}}};
    }

    string content() const
    {
        ifstream stream(m_file);
        stringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    /// The worker is its own thread; give it a moment to reach the file.
    string waitForContent(const size_t lines) const
    {
        const auto deadline = chrono::steady_clock::now() + chrono::seconds(3);
        while (chrono::steady_clock::now() < deadline)
        {
            const auto text = content();
            if (ranges::count(text, '\n') >= static_cast<ptrdiff_t>(lines))
            {
                return text;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        return content();
    }

    FileLogEngine    m_logEngine {"tap-tests.log"};
    filesystem::path m_file;
};

} // namespace

TEST_F(TapTests, deliversOnlyMatchingTopicsWithBinaryPayloads)
{
    ExtensionHost host(m_logEngine, "tap");
    host.start({extension()});
    ASSERT_TRUE(host.tapping());

    const char payload[] = {'a', '\0', 'b', static_cast<char>(0xFF)};
    host.publishMessage("client-1", "user", "sensors/1", payload, sizeof(payload), 1, false);
    host.publishMessage("client-1", "user", "other/1", payload, sizeof(payload), 0, false);

    const auto text = waitForContent(1);
    EXPECT_NE(string::npos, text.find("sensors/1"));
    EXPECT_EQ(string::npos, text.find("other/1"));
    EXPECT_NE(string::npos, text.find("610062ff")) << text;

    host.stop();
}

TEST_F(TapTests, dropsWhatExceedsTheTapPayloadLimit)
{
    ExtensionHost host(m_logEngine, "tap");
    host.start({extension("sensors/#", "2")});
    ASSERT_TRUE(host.tapping());

    const char payload[] = {'a', 'b', 'c'};
    host.publishMessage("client", "user", "sensors/1", payload, sizeof(payload), 0, false);

    this_thread::sleep_for(chrono::milliseconds(100));
    EXPECT_TRUE(content().empty()) << content();

    // Counted, because a drop is silent by design: the queue is bounded so the broker never waits,
    // and this counter is the only way to learn that a tap is losing messages.
    // Found by name: the extension directory holds the rest of the suite's fixtures too, and
    // discovery lists every one of them as stopped.
    const auto described = host.describe();
    const auto found = ranges::find_if(described, [](const auto& one)
    {
        return one.m_name == "tap-recorder";
    });
    ASSERT_NE(found, described.end());
    EXPECT_EQ(0U, found->m_messagesDelivered);
    EXPECT_EQ(1U, found->m_messagesDropped);

    host.stop();
}

TEST_F(TapTests, countsWhatTheTapDelivered)
{
    ExtensionHost host(m_logEngine, "tap");
    host.start({extension()});
    ASSERT_TRUE(host.tapping());

    const char payload[] = {'x'};
    host.publishMessage("client", "user", "sensors/1", payload, sizeof(payload), 0, false);
    EXPECT_FALSE(waitForContent(1).empty());

    const auto described = host.describe();
    const auto found = ranges::find_if(described, [](const auto& one)
    {
        return one.m_name == "tap-recorder";
    });
    ASSERT_NE(found, described.end());
    EXPECT_EQ(1U, found->m_messagesDelivered);
    EXPECT_EQ(0U, found->m_messagesDropped);

    host.stop();
}

TEST_F(TapTests, aSlowTapDoesNotBlockThePublisher)
{
    ExtensionHost host(m_logEngine, "tap");
    host.start({extension("sensors/#", "0", "50")});
    ASSERT_TRUE(host.tapping());

    const char payload[] = {'x'};
    const auto started = chrono::steady_clock::now();
    for (size_t at = 0; at < 10; ++at)
    {
        host.publishMessage("client", "user", "sensors/1", payload, sizeof(payload), 0, false);
    }
    const auto elapsed = chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, chrono::milliseconds(100)) << "the publisher waited for the tap";

    host.stop();
}

TEST_F(TapTests, aStoppedExtensionReceivesNothing)
{
    ExtensionHost host(m_logEngine, "tap");
    host.start({extension()});
    host.stop();

    const char payload[] = {'x'};
    host.publishMessage("client", "user", "sensors/1", payload, sizeof(payload), 0, false);
    this_thread::sleep_for(chrono::milliseconds(100));
    EXPECT_TRUE(content().empty()) << content();
}
