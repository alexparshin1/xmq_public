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

#include "server/HostMetrics.h"

#include <gtest/gtest.h>
#include <thread>

using namespace std;
using namespace xmq;

namespace {

// Long enough for the CPU counters to move by a measurable amount, short enough not to slow the
// suite noticeably.
constexpr auto samplingInterval = 250ms;

} // namespace

// The figures must be readable at all, and be of the right order: a dashboard showing zero
// memory or zero disk is indistinguishable from one that is broken.
TEST(XMQ_HostMetrics, readsHostFigures)
{
    HostMetrics metrics;

    const auto snapshot = metrics.read();

    ASSERT_TRUE(snapshot.m_available) << "No host figures could be read on this platform";

    EXPECT_GT(snapshot.m_memoryTotal, 0U);
    EXPECT_GT(snapshot.m_memoryAvailable, 0U);
    EXPECT_LE(snapshot.m_memoryAvailable, snapshot.m_memoryTotal);
    EXPECT_GT(snapshot.m_memoryUsedByProcess, 0U) << "This test process must be using some memory";

    // Memory in use is reported as the familiar tools report it - neither free nor cache - so it
    // cannot exceed the machine, and cannot be confused with "everything that is not available":
    // on a host with unreclaimable cache the second is much the larger of the two.
    EXPECT_GT(snapshot.m_memoryUsed, 0U);
    EXPECT_LE(snapshot.m_memoryUsed, snapshot.m_memoryTotal);
    EXPECT_LE(snapshot.m_memoryUsed, snapshot.m_memoryTotal - snapshot.m_memoryAvailable + snapshot.m_memoryTotal / 2)
        << "In-use memory is implausibly far above what is unavailable";
}

// The first read has no interval behind it, so its CPU percentages are zero rather than a
// number invented from a single sample. The second read covers a real interval.
TEST(XMQ_HostMetrics, cpuNeedsTwoSamples)
{
    HostMetrics metrics;

    const auto first = metrics.read();
    EXPECT_DOUBLE_EQ(0.0, first.m_cpuPercentTotal) << "A single sample cannot yield a percentage";
    EXPECT_DOUBLE_EQ(0.0, first.m_cpuPercentProcess);

    this_thread::sleep_for(samplingInterval);

    const auto second = metrics.read();

    // Machine-wide use is a share of every core together, so it cannot exceed 100%.
    EXPECT_GE(second.m_cpuPercentTotal, 0.0);
    EXPECT_LE(second.m_cpuPercentTotal, 100.0);

    // The process is measured against elapsed time, so on a multi-core host it may exceed 100% -
    // that is the point of it, since it says how many cores are in use. It cannot be negative,
    // and cannot exceed what the machine has.
    EXPECT_GE(second.m_cpuPercentProcess, 0.0);
    EXPECT_LE(second.m_cpuPercentProcess, 100.0 * static_cast<double>(thread::hardware_concurrency() + 1));

    // The core count is what lets the process figure be shown on the same scale as the machine's,
    // so an absent one would leave that gauge stuck at zero.
    EXPECT_GT(second.m_cpuCores, 0U);
    EXPECT_LE(second.m_cpuPercentProcess, 100.0 * static_cast<double>(second.m_cpuCores) + 100.0);
}

// Uptime is measured from construction and must not go backwards.
TEST(XMQ_HostMetrics, uptimeAdvances)
{
    HostMetrics metrics;

    const auto first = metrics.read();
    this_thread::sleep_for(samplingInterval);
    const auto second = metrics.read();

    EXPECT_GE(second.m_processUptimeSeconds, first.m_processUptimeSeconds);
}

// Disk is read for whichever directory the caller names - the Redis database directory, when
// Redis is on this host and says where it is.
TEST(XMQ_HostMetrics, readsDiskSpaceOfADirectory)
{
    uint64_t available = 0;
    uint64_t total = 0;

    ASSERT_TRUE(HostMetrics::readDiskSpace(filesystem::temp_directory_path(), available, total));
    EXPECT_GT(total, 0U);
    EXPECT_LE(available, total);
}

// A directory that does not exist must report failure rather than throw: this is read behind a
// web request, where an exception is a failed dashboard, and a Redis that moved its database is
// an ordinary thing to meet.
TEST(XMQ_HostMetrics, missingDirectoryIsNotFatal)
{
    uint64_t available = 1;
    uint64_t total = 1;

    bool read = true;
    EXPECT_NO_THROW(read = HostMetrics::readDiskSpace("/no/such/directory/for/xmq/tests", available, total));
    EXPECT_FALSE(read);

    // An empty path is the "Redis did not tell us" case, and must be equally harmless.
    EXPECT_NO_THROW(read = HostMetrics::readDiskSpace("", available, total));
    EXPECT_FALSE(read);
}
