#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace xmq {

// Each send queue has exactly one consumer. Checking the empty transition under the same
// mutex as the consumer's wait predicate makes it safe to omit notifications for backlogs.
template<class T>
class SendSessionQueue
{
public:
    struct Stats
    {
        uint64_t enqueued = 0;
        uint64_t notifications = 0;
        uint64_t emptyPops = 0;
        uint64_t rounds = 0;
    };

    SendSessionQueue(bool coalesce, bool measure) : m_coalesce(coalesce), m_measure(measure) {}

    void push(T item)
    {
        bool notify;
        {
            const std::scoped_lock lock(m_mutex);
            notify = !m_coalesce || m_queue.empty();
            m_queue.push_back(std::move(item));
            if (m_measure)
            {
                ++m_stats.enqueued;
                m_stats.notifications += notify;
            }
        }
        if (notify)
        {
            m_ready.notify_one();
        }
    }

    bool pop(std::vector<T>& items, size_t limit, std::chrono::milliseconds timeout)
    {
        // Releasing the previous round's references can call back into the queue.
        items.clear();
        std::unique_lock lock(m_mutex);
        if (m_measure && m_queue.empty())
        {
            ++m_stats.emptyPops;
        }
        if (!m_ready.wait_for(lock, timeout, [this] { return !m_queue.empty(); }))
        {
            return false;
        }
        while (!m_queue.empty() && items.size() < limit)
        {
            items.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        if (m_measure)
        {
            ++m_stats.rounds;
        }
        return !items.empty();
    }

    size_t size() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_queue.size();
    }

    void clear()
    {
        std::deque<T> discarded;
        {
            const std::scoped_lock lock(m_mutex);
            discarded.swap(m_queue);
        }
    }

    Stats takeStats()
    {
        const std::scoped_lock lock(m_mutex);
        return std::exchange(m_stats, {});
    }

    bool measuring() const { return m_measure; }

private:
    const bool m_coalesce;
    const bool m_measure;
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<T> m_queue;
    Stats m_stats;
};

} // namespace xmq
