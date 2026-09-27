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

#include <gtest/gtest.h>

#include "base/ReceiveSteering.h"

#include <filesystem>
#include <fstream>

using namespace std;
using namespace xmq;

namespace fs = std::filesystem;

namespace {

/// A network interface as /sys/class/net describes it, built under the temporary directory.
class FakeInterface
{
public:
    FakeInterface()
        : m_root(fs::temp_directory_path() /
                 ("xmq-receive-steering-" + string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
        , m_directory(m_root / "eth0")
    {
        fs::remove_all(m_root);
        fs::create_directories(m_directory / "queues");
    }

    FakeInterface(const FakeInterface&) = delete;
    FakeInterface& operator=(const FakeInterface&) = delete;

    ~FakeInterface()
    {
        error_code error;
        fs::remove_all(m_root, error);
    }

    FakeInterface& physical()
    {
        fs::create_directories(m_directory / "device");
        return *this;
    }

    FakeInterface& operstate(const string& state)
    {
        write(m_directory / "operstate", state);
        return *this;
    }

    /// An empty mask leaves the rps_cpus file out, which is what an unreadable one looks like.
    FakeInterface& receiveQueue(const int index, const string& mask)
    {
        const auto queue = m_directory / "queues" / ("rx-" + to_string(index));
        fs::create_directories(queue);
        if (!mask.empty())
        {
            write(queue / "rps_cpus", mask);
        }
        return *this;
    }

    FakeInterface& transmitQueue(const int index)
    {
        fs::create_directories(m_directory / "queues" / ("tx-" + to_string(index)));
        return *this;
    }

    [[nodiscard]] string directory() const
    {
        return m_directory.string();
    }

private:
    static void write(const fs::path& path, const string& text)
    {
        ofstream(path) << text << "\n";
    }

    fs::path m_root;
    fs::path m_directory;
};

constexpr size_t fourCpus = 4;

} // namespace

TEST(ReceiveSteeringTests, SingleQueueUnsteeredCardIsReported)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "00").transmitQueue(0);

    size_t receiveQueues = 0;
    EXPECT_TRUE(ReceiveSteering::needsSteering(card.directory(), fourCpus, receiveQueues));
    EXPECT_EQ(receiveQueues, 1U);
}

TEST(ReceiveSteeringTests, EmptyMaskOnLargeMachineIsRecognised)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "00000000,00000000");

    size_t receiveQueues = 0;
    EXPECT_TRUE(ReceiveSteering::needsSteering(card.directory(), 64, receiveQueues));
}

TEST(ReceiveSteeringTests, SteeredCardIsNotReported)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "70");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(card.directory(), fourCpus, receiveQueues));
}

TEST(ReceiveSteeringTests, MultiQueueCardIsNotReported)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "00").receiveQueue(1, "00");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(card.directory(), fourCpus, receiveQueues));
    EXPECT_EQ(receiveQueues, 2U);
}

TEST(ReceiveSteeringTests, VirtualInterfaceIsNotReported)
{
    FakeInterface bridge;
    bridge.operstate("up").receiveQueue(0, "00");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(bridge.directory(), fourCpus, receiveQueues));
}

TEST(ReceiveSteeringTests, InterfaceThatIsDownIsNotReported)
{
    FakeInterface card;
    card.physical().operstate("down").receiveQueue(0, "00");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(card.directory(), fourCpus, receiveQueues));
}

TEST(ReceiveSteeringTests, TwoCpusLeaveNothingToSteerTo)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "00");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(card.directory(), 2, receiveQueues));
}

TEST(ReceiveSteeringTests, UnreadableMaskIsNotReported)
{
    FakeInterface card;
    card.physical().operstate("up").receiveQueue(0, "");

    size_t receiveQueues = 0;
    EXPECT_FALSE(ReceiveSteering::needsSteering(card.directory(), fourCpus, receiveQueues));
}

#ifdef __linux__
TEST(ReceiveSteeringTests, LoopbackIsNeverReported)
{
    // The real sysfs: loopback has no card behind it, so it can never be a gap.
    EXPECT_TRUE(ReceiveSteering::findGaps({"127.0.0.1"}, fourCpus).empty());
}
#endif
