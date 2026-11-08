/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "server/JsonLogEngine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std;
using namespace std::chrono_literals;
using namespace sptk;
using namespace xmq;

namespace {

/// Stands in for the file engine: keeps what the wrapper would have written.
class CapturingEngine final : public LogEngine
{
public:
    CapturingEngine()
        : LogEngine("CapturingEngine")
    {
    }

    bool saveMessage(const Logger::Message& message) override
    {
        m_last = message.message;
        m_saved = true;
        return true;
    }

    [[nodiscard]] bool saved() const { return m_saved.load(); }
    [[nodiscard]] const string& last() const { return m_last; }

private:
    atomic<bool> m_saved {false};
    string       m_last;
};

/// The wrapper formats on its own thread; wait for the line rather than sleeping a fixed time.
bool waitForLine(const CapturingEngine& engine, string& line)
{
    for (auto attempt = 0; attempt < 200; ++attempt)
    {
        if (engine.saved())
        {
            line = engine.last();
            return true;
        }
        this_thread::sleep_for(5ms);
    }
    return false;
}

} // namespace

TEST(JsonLogEngineTests, WritesOneObjectWithTheFieldsTheBrokerKnows)
{
    CapturingEngine inner;
    {
        JsonLogEngine json(shared_ptr<LogEngine>(&inner, [](LogEngine*) {}), "node-a");
        json.option(LogEngine::Option::ENABLE, true);

        Logger logger(json, "(node-a) ");
        logger.log(LogPriority::Info, "<server_events> Started.");

        string line;
        ASSERT_TRUE(waitForLine(inner, line)) << "no line was written";
        EXPECT_NE(string::npos, line.find("\"level\":\"info\"")) << line;
        EXPECT_NE(string::npos, line.find("\"node\":\"node-a\"")) << line;
        EXPECT_NE(string::npos, line.find("\"subject\":\"server_events\"")) << line;
        EXPECT_NE(string::npos, line.find("\"message\":\"Started.\"")) << line;
        EXPECT_NE(string::npos, line.find("\"timestamp\":\"")) << line;
    }
}

TEST(JsonLogEngineTests, KeepsDirectLoggersInScopeAndEscapesTheirText)
{
    CapturingEngine inner;
    {
        JsonLogEngine json(shared_ptr<LogEngine>(&inner, [](LogEngine*) {}), "node-a");
        json.option(LogEngine::Option::ENABLE, true);

        Logger logger(json, "[Startup] ");
        logger.log(LogPriority::Warning, "A \"quoted\" line\nwith a newline.");

        string line;
        ASSERT_TRUE(waitForLine(inner, line)) << "no line was written";
        EXPECT_NE(string::npos, line.find("\"scope\":\"Startup\"")) << line;
        EXPECT_NE(string::npos, line.find("\\\"quoted\\\"")) << line;
        EXPECT_NE(string::npos, line.find("\\nwith a newline.")) << line;
        // One record, one line: a raw newline would split the JSON over two.
        EXPECT_EQ(string::npos, line.find('\n')) << line;
    }
}
