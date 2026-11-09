/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "server/Subscription/SubscriptionManager.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

using namespace std;
using namespace xmq;

namespace {

/**
 * @brief A session as the allocator hands it out: one size class, so the addresses of a fan-out
 *        lie on a stride instead of anywhere in the address space.
 */
class SessionSized
{
public:
    virtual ~SessionSized() = default;

private:
    std::byte m_padding[512] {};
};

} // namespace

/**
 * The fan-out arrives as sessions allocated one after another from a single size class, and their
 * addresses differ by a stride that is a multiple of the page and of the alignments the shard
 * counts divide. Folding the address directly put every subscriber of a thousand in one shard,
 * which is how the whole fan-out came to run on one delivery thread however many were configured.
 */
TEST(ShardMappingTests, SpreadsOneAllocatorSizeClassOverEveryShard)
{
    constexpr size_t sessionCount = 1000;

    vector<unique_ptr<SessionSized>> sessions;
    sessions.reserve(sessionCount);
    for (size_t i = 0; i < sessionCount; ++i)
    {
        sessions.emplace_back(make_unique<SessionSized>());
    }

    // The counts the delivery threads actually configure, and the one below them.
    for (const size_t shardCount : {size_t{2}, size_t{4}, size_t{8}})
    {
        vector<size_t> perShard(shardCount, 0);
        for (const auto& session : sessions)
        {
            // Only the address is read, which is what the mapping is about; a session needs no
            // subscriptions to be counted.
            const auto address = reinterpret_cast<const ISubscriptionClient*>(session.get());
            ++perShard[SubscriptionManager::shardOf(address, shardCount)];
        }

        // Not just "not one shard": a mapping that folded a stride to two remainders would still
        // leave half the threads idle.
        for (size_t shard = 0; shard < shardCount; ++shard)
        {
            EXPECT_GE(perShard[shard], sessionCount / 20)
                << "Shard " << shard << " of " << shardCount << " got " << perShard[shard]
                << " of " << sessionCount << " sessions of one size class";
        }
    }
}
