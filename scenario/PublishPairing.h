#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xmq {

/// One send timestamp per sequence. Fan-out tracks which destinations already received it.
class PublishPairing
{
public:
    using Clock = std::chrono::steady_clock;

    void sent(uint32_t sequence, size_t deliveries, std::optional<size_t> destination = {})
    {
        if (deliveries == 0)
        {
            return;
        }
        Entry entry {Clock::now(), deliveries, deliveries, destination, {}};
        if (deliveries > 1)
        {
            entry.seen.resize((deliveries + 63) / 64);
        }
        auto& shard = forSequence(sequence);
        const std::scoped_lock lock(shard.mutex);
        shard.entries.insert_or_assign(sequence, std::move(entry));
    }

    std::optional<Clock::time_point> received(uint32_t sequence, size_t subscriber)
    {
        auto& shard = forSequence(sequence);
        const std::scoped_lock lock(shard.mutex);
        const auto it = shard.entries.find(sequence);
        if (it == shard.entries.end())
        {
            return {};
        }
        auto& entry = it->second;
        if (entry.destination && *entry.destination != subscriber)
        {
            return {};
        }
        if (!entry.seen.empty())
        {
            if (subscriber >= entry.destinations)
            {
                return {};
            }
            const auto mask = uint64_t{1} << (subscriber % 64);
            auto& word = entry.seen[subscriber / 64];
            if (word & mask)
            {
                return {};
            }
            word |= mask;
        }
        const auto at = entry.at;
        if (--entry.remaining == 0)
        {
            shard.entries.erase(it);
        }
        return at;
    }

    void cancel(uint32_t sequence)
    {
        auto& shard = forSequence(sequence);
        const std::scoped_lock lock(shard.mutex);
        shard.entries.erase(sequence);
    }

private:
    struct Entry
    {
        Clock::time_point at;
        size_t remaining;
        size_t destinations;
        std::optional<size_t> destination;
        std::vector<uint64_t> seen;
    };
    struct Shard
    {
        std::mutex mutex;
        std::unordered_map<uint32_t, Entry> entries;
    };
    std::array<Shard, 64> m_shards;
    Shard& forSequence(uint32_t sequence) { return m_shards[sequence % m_shards.size()]; }
};

} // namespace xmq
