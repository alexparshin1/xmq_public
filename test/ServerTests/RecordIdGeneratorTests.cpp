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

#include "base/RecordIdGenerator.h"
#include <sptk5/threads/JoiningThread.h>
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

using namespace xmq;

TEST(RecordIdGeneratorTests, DefaultConstructor)
{
    RecordIdGenerator generator;

    EXPECT_EQ(generator.nextRecordId(), 1U);
}

TEST(RecordIdGeneratorTests, nextRecordIdSequential)
{
    RecordIdGenerator generator;

    EXPECT_EQ(generator.nextRecordId(), 1U);
    EXPECT_EQ(generator.nextRecordId(), 2U);
    EXPECT_EQ(generator.nextRecordId(), 3U);
    EXPECT_EQ(generator.nextRecordId(), 4U);
    EXPECT_EQ(generator.nextRecordId(), 5U);
}

TEST(RecordIdGeneratorTests, Reset)
{
    RecordIdGenerator generator;

    EXPECT_EQ(generator.nextRecordId(), 1U);
    EXPECT_EQ(generator.nextRecordId(), 2U);
    EXPECT_EQ(generator.nextRecordId(), 3U);

    generator.setLastRecordId(0);

    EXPECT_EQ(generator.nextRecordId(), 1U);
    EXPECT_EQ(generator.nextRecordId(), 2U);
}

TEST(RecordIdGeneratorTests, ThreadSafety)
{
    RecordIdGenerator               generator;
    constexpr auto                  numThreads = 10U;
    constexpr auto                  idsPerThread = 100U;
    sptk::JoiningThreads       threads;
    std::vector<std::set<uint64_t>> threadIds(numThreads);

    for (size_t i = 0; i < numThreads; ++i)
    {
        threads.emplace_back([&generator, &threadIds, i]()
                             {
                                 for (size_t j = 0; j < idsPerThread; ++j)
                                 {
                                     threadIds[i].insert(generator.nextRecordId());
                                 }
                             });
    }

    for (auto& thread: threads)
    {
        thread.join();
    }

    std::set<uint64_t> allIds;
    for (const auto& ids: threadIds)
    {
        EXPECT_EQ(ids.size(), idsPerThread);
        allIds.insert(ids.begin(), ids.end());
    }

    EXPECT_EQ(allIds.size(), numThreads * idsPerThread);
}

TEST(RecordIdGeneratorTests, OverflowBehavior)
{
    RecordIdGenerator generator;

    constexpr auto maxValue = std::numeric_limits<uint64_t>::max();

    for (uint64_t i = 1; i < 100; ++i)
    {
        generator.nextRecordId();
    }

    EXPECT_LT(generator.nextRecordId(), maxValue);
}

TEST(RecordIdGeneratorTests, MultipleInstancesIndependent)
{
    RecordIdGenerator generator1;
    RecordIdGenerator generator2;

    EXPECT_EQ(generator1.nextRecordId(), 1U);
    EXPECT_EQ(generator2.nextRecordId(), 1U);

    EXPECT_EQ(generator1.nextRecordId(), 2U);
    EXPECT_EQ(generator1.nextRecordId(), 3U);

    EXPECT_EQ(generator2.nextRecordId(), 2U);

    EXPECT_EQ(generator1.nextRecordId(), 4U);
    EXPECT_EQ(generator2.nextRecordId(), 3U);
}

TEST(RecordIdGeneratorTests, ResetAfterManyIds)
{
    RecordIdGenerator generator;

    for (auto i = 0; i < 10000; ++i)
    {
        generator.nextRecordId();
    }

    generator.setLastRecordId(0);

    EXPECT_EQ(generator.nextRecordId(), 1U);
    EXPECT_EQ(generator.nextRecordId(), 2U);
}
