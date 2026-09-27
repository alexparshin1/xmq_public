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

// The storage clock is the reference every node in a cluster compares timestamps against, so it
// has to come from the backend rather than from whichever host happens to be asking. These tests
// assert that Storage tracks the backend clock - deliberately not that the backend and the host
// agree, which is a property of the environment and not of this code.

#include "StorageTests.h"
#include "storage/RedisConnection.h"
#include "storage/Storage.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_F(XMQ_StorageTests, storageClockTracksBackend)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);
    const auto storage = testStorage->storage();
    ASSERT_TRUE(storage);

    const auto backendTime = RedisConnection::readServerTime(storage->getRedis());
    ASSERT_FALSE(backendTime.zero()) << "Could not read the time from Redis";

    const auto storageTime = storage->getCurrentTime();
    ASSERT_FALSE(storageTime.zero());

    // Both readings come from the backend's clock, so they must agree closely whatever the host
    // clock is doing - that is the whole point of the offset.
    constexpr int64_t maxSkewMs = 1000;
    const auto        skewMs = llabs(chrono::duration_cast<chrono::milliseconds>(storageTime - backendTime).count());
    EXPECT_LT(skewMs, maxSkewMs) << "Storage clock is " << skewMs << " ms away from the backend clock";
}

TEST_F(XMQ_StorageTests, storageClockDoesNotQueryBackendPerCall)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);
    const auto storage = testStorage->storage();
    ASSERT_TRUE(storage);

    // Prime the offset so the first sample is not counted.
    (void) storage->getCurrentTime();

    constexpr int callCount = 1000;
    Stopwatch     stopwatch;
    stopwatch.start();
    for (int i = 0; i < callCount; ++i)
    {
        (void) storage->getCurrentTime();
    }
    stopwatch.stop();

    // A round trip per call would be milliseconds apiece; applying a cached offset is nanoseconds.
    // The threshold is loose on purpose - it is here to catch a per-call query, not to benchmark.
    constexpr double maxTotalMs = 100.0;
    EXPECT_LT(stopwatch.milliseconds(), maxTotalMs)
        << callCount << " calls took " << stopwatch.milliseconds() << " ms, which suggests every call queries the backend";
}

TEST_F(XMQ_StorageTests, storageClockAdvances)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);
    const auto storage = testStorage->storage();
    ASSERT_TRUE(storage);

    const auto first = storage->getCurrentTime();
    this_thread::sleep_for(50ms);
    const auto second = storage->getCurrentTime();

    // Between samples the offset is held constant and host time carries the clock forward, so it
    // has to keep moving rather than freezing at whatever the last backend reading was.
    EXPECT_GT(second, first);
}
