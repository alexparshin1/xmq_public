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


#include "server/Settings/VerificationCache.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>

using namespace std;
using namespace xmq;

TEST(XMQ_VerificationCache, answersUnknownUntilSomethingIsRemembered)
{
    const VerificationCache cache;
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("dave", "Sunflower7!"));
}

TEST(XMQ_VerificationCache, remembersWhatVerifyingAnswered)
{
    VerificationCache cache;

    cache.remember("dave", "Sunflower7!", true);
    cache.remember("erin", "Marigold3?", false);

    EXPECT_EQ(VerificationCache::Answer::Allowed, cache.look("dave", "Sunflower7!"));

    // Refusals are kept too, so that wrong passwords cannot be used to spend the broker's cores.
    EXPECT_EQ(VerificationCache::Answer::Refused, cache.look("erin", "Marigold3?"));
}

TEST(XMQ_VerificationCache, anotherPasswordIsAnotherEntry)
{
    VerificationCache cache;
    cache.remember("dave", "Sunflower7!", true);

    // The one that matters: a remembered admission must not admit a different password. It is also
    // what makes a corrected password cost nothing to fix - it was never the entry that was wrong.
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("dave", "Sunflower8!"));
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("davex", "Sunflower7!"));
}

TEST(XMQ_VerificationCache, theSeparatorKeepsTheNameAndThePasswordApart)
{
    VerificationCache cache;
    cache.remember("ab", "cd", true);

    // Without a separator between them these are one set of credentials, and the remembered
    // admission of one account would admit another.
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("a", "bcd"));
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("abc", "d"));
}

TEST(XMQ_VerificationCache, aChangeToTheAccountsDropsEverything)
{
    VerificationCache cache;
    cache.remember("dave", "Sunflower7!", true);
    ASSERT_EQ(VerificationCache::Answer::Allowed, cache.look("dave", "Sunflower7!"));

    cache.forget();

    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("dave", "Sunflower7!"));
}

TEST(XMQ_VerificationCache, theTableFollowsTheNumberOfAccounts)
{
    VerificationCache cache;
    const auto        smallest = cache.capacity();

    cache.sizeFor(4);
    EXPECT_EQ(smallest, cache.capacity()) << "a handful of accounts does not shrink it below its floor";

    cache.sizeFor(100000);
    EXPECT_GE(cache.capacity(), 200000U) << "a field of devices gets room for its own accounts";

    // The one this exists for: nothing a store reports can ask for all of memory.
    cache.sizeFor(1000000000U);
    EXPECT_LE(cache.capacity(), 1U << 20U);
}

TEST(XMQ_VerificationCache, resizingDropsWhatWasRemembered)
{
    VerificationCache cache;
    cache.remember("dave", "Sunflower7!", true);

    cache.sizeFor(100000);

    // A resize happens when the accounts are loaded, which is exactly when what was remembered
    // about them stops being trustworthy.
    EXPECT_EQ(VerificationCache::Answer::Unknown, cache.look("dave", "Sunflower7!"));
}

// Not run with the suite: it measures rather than asserts, and what it measures is the whole reason
// the cache exists. Run it by name.
TEST(XMQ_VerificationCache, DISABLED_lookupCost)
{
    VerificationCache cache;
    cache.sizeFor(100000);
    cache.remember("dave", "Sunflower7!", true);

    constexpr size_t rounds = 200000;

    const auto started = chrono::steady_clock::now();
    size_t     hits = 0;
    for (size_t round = 0; round < rounds; ++round)
    {
        hits += cache.look("dave", "Sunflower7!") == VerificationCache::Answer::Allowed ? 1 : 0;
    }
    const auto elapsed = chrono::duration<double>(chrono::steady_clock::now() - started).count();

    ASSERT_EQ(rounds, hits);
    cout << "cache hit: " << elapsed / rounds * 1e6 << " us, " << size_t(rounds / elapsed)
         << " per second on one thread" << endl;
}
