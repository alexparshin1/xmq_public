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

#include "common/SessionCounters.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace xmq;

TEST(SessionCountersTests, DefaultConstructor)
{
    SessionCounters counters;

    EXPECT_EQ(counters.getPublishSendCount(), 0U);
    EXPECT_EQ(counters.getPublishReceiveCount(), 0U);
    EXPECT_EQ(counters.getPublishScheduledCount(), 0U);
}

TEST(SessionCountersTests, IncrementPublishSendCount)
{
    SessionCounters counters;

    counters.incrementPublishSendCount();
    EXPECT_EQ(counters.getPublishSendCount(), 1U);

    counters.incrementPublishSendCount();
    EXPECT_EQ(counters.getPublishSendCount(), 2U);
}

TEST(SessionCountersTests, IncrementPublishReceiveCount)
{
    SessionCounters counters;

    counters.incrementPublishReceiveCount();
    EXPECT_EQ(counters.getPublishReceiveCount(), 1U);

    counters.incrementPublishReceiveCount();
    EXPECT_EQ(counters.getPublishReceiveCount(), 2U);
}

TEST(SessionCountersTests, IncrementPublishScheduledCount)
{
    SessionCounters counters;

    counters.incrementPublishScheduledCount();
    EXPECT_EQ(counters.getPublishScheduledCount(), 1U);

    counters.incrementPublishScheduledCount();
    EXPECT_EQ(counters.getPublishScheduledCount(), 2U);
}

TEST(SessionCountersTests, ResetCounters)
{
    SessionCounters counters;

    counters.incrementPublishSendCount();
    counters.incrementPublishReceiveCount();
    counters.incrementPublishScheduledCount();

    counters.resetCounters();

    EXPECT_EQ(counters.getPublishSendCount(), 0U);
    EXPECT_EQ(counters.getPublishReceiveCount(), 0U);
    EXPECT_EQ(counters.getPublishScheduledCount(), 0U);
}

TEST(SessionCountersTests, MultipleIncrements)
{
    SessionCounters counters;

    for (auto i = 0; i < 100; ++i)
    {
        counters.incrementPublishSendCount();
    }

    EXPECT_EQ(counters.getPublishSendCount(), 100U);

    for (auto i = 0; i < 50; ++i)
    {
        counters.incrementPublishReceiveCount();
    }

    EXPECT_EQ(counters.getPublishReceiveCount(), 50U);
}

TEST(SessionCountersTests, CountersAreIndependent)
{
    SessionCounters counters;

    counters.incrementPublishSendCount();
    counters.incrementPublishReceiveCount();
    counters.incrementPublishScheduledCount();

    EXPECT_EQ(counters.getPublishSendCount(), 1U);
    EXPECT_EQ(counters.getPublishReceiveCount(), 1U);
    EXPECT_EQ(counters.getPublishScheduledCount(), 1U);
}

TEST(SessionCountersTests, ThreadSafetyConcurrentIncrements)
{
    SessionCounters counters;
    constexpr auto  numThreads = 10U;
    constexpr auto  incrementsPerThread = 1000U;

    std::vector<std::thread> threads;

    for (size_t i = 0; i < numThreads; ++i)
    {
        threads.emplace_back([&counters]()
                             {
                                 for (size_t j = 0; j < incrementsPerThread; ++j)
                                 {
                                     counters.incrementPublishSendCount();
                                     counters.incrementPublishReceiveCount();
                                     counters.incrementPublishScheduledCount();
                                 }
                             });
    }

    for (auto& thread: threads)
    {
        thread.join();
    }

    EXPECT_EQ(counters.getPublishSendCount(), numThreads * incrementsPerThread);
    EXPECT_EQ(counters.getPublishReceiveCount(), numThreads * incrementsPerThread);
    EXPECT_EQ(counters.getPublishScheduledCount(), numThreads * incrementsPerThread);
}
