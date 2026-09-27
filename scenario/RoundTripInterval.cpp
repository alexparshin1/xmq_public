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

#include "RoundTripInterval.h"

#include <ranges>

using namespace std;
using namespace chrono;
using namespace sptk;
using namespace xmq;

RoundTripLatency::RoundTripLatency(const RoundTripLatency& other)
{
    const std::scoped_lock lock(other.m_mutex);
    m_groupIntervalDuration = other.m_groupIntervalDuration;
    m_startTime = other.m_startTime;
    m_nextIntervalTime = other.m_nextIntervalTime;
    m_intervals = other.m_intervals;
    m_summaryLatency = other.m_summaryLatency;
    m_count = other.m_count;
}

void RoundTripLatency::start(const microseconds groupInterval)
{
    m_groupIntervalDuration = groupInterval;

    m_startTime = Clock::now();
    m_nextIntervalTime = m_startTime + m_groupIntervalDuration;

    m_summaryLatency = microseconds(0);
    m_count = 0;
    m_intervals.clear();
}

void RoundTripLatency::stop()
{
    const std::scoped_lock lock(m_mutex);
    storeCurrentInterval();
}

void RoundTripLatency::sent()
{
    const std::scoped_lock lock(m_mutex);
    m_started.push_back(duration_cast<microseconds>(Clock::now().time_since_epoch()));
}

void RoundTripLatency::storeCurrentInterval()
{
    if (m_count)
    {
        // Key the bucket by the wall-clock start of the interval being closed, so buckets stay
        // aligned across objects and across delivery gaps (m_intervals.size() would drift after
        // an interval with no samples).
        const auto interval = duration_cast<microseconds>(m_nextIntervalTime - m_groupIntervalDuration - m_startTime);
        auto& [count, summaryLatency] = m_intervals[interval];
        count += m_count;
        summaryLatency += m_summaryLatency;
        m_summaryLatency = {};
        m_count = 0;
    }
    m_nextIntervalTime += m_groupIntervalDuration;
}

void RoundTripLatency::received()
{
    const std::scoped_lock lock(m_mutex);

    // Non-blocking pop: on paths that never call sent() (e.g. message delivery, where
    // latency is measured via LatencyTrace instead) the queue is empty, and waiting here
    // would stall every delivery callback for the full timeout while holding m_mutex.
    if (microseconds started;
        m_started.pop_front(started, milliseconds(0)))
    {
        const auto now = Clock::now();

        const auto latency = duration_cast<microseconds>(now.time_since_epoch()) - started;
        while (m_groupIntervalDuration.count() > 0 && now > m_nextIntervalTime)
        {
            storeCurrentInterval();
        }

        m_summaryLatency += latency;
        ++m_count;
    }
}

void RoundTripLatency::add(const microseconds duration)
{
    const std::scoped_lock lock(m_mutex);
    m_summaryLatency += duration;
    ++m_count;
}

size_t RoundTripLatency::count() const
{
    const std::scoped_lock lock(m_mutex);
    return m_count;
}

microseconds RoundTripLatency::averageLatency() const
{
    const std::scoped_lock lock(m_mutex);
    microseconds           summaryLatency {0};
    size_t                 count {0};
    for (const auto& [_count, _summaryLatency]: m_intervals | std::views::values)
    {
        summaryLatency += _summaryLatency;
        count += _count;
    }

    if (count)
    {
        return summaryLatency / count;
    }

    return {};
}

void RoundTripLatency::print(const string& scenarioTitle) const
{
    const std::scoped_lock lock(m_mutex);
    COUT("Scenario: " << scenarioTitle);
    COUT(std::format("{:<10} {:>10} {:>10} {:>12}", "Interval", "Count", "Latency", "Rate/s"));
#ifdef _WIN32
    COUT("---------------------------------------------");
#else
    COUT("─────────────────────────────────────────────");
#endif
    // Rate is count divided by wall-clock bucket width, not tied to any configured publish
    // rate - this is what shows the actual delivered throughput once backpressure (e.g.
    // max_inflight_messages) makes publishers self-throttle below the nominal target rate.
    const auto           intervalSeconds = duration<double>(m_groupIntervalDuration).count();
    size_t               totalCount = 0;
    microseconds         totalLatency {0};
    vector<microseconds> intervalAverageLatencies;
    for (const auto& [interval, intervalData]: m_intervals)
    {
        totalCount += intervalData.m_count;
        totalLatency += intervalData.m_summaryLatency;
        if (intervalData.m_count == 0)
        {
            COUT(format("{:<10} {:>10d} {:>10} {:>12}", duration_cast<milliseconds>(interval), intervalData.m_count, "?", "?"));
        }
        else
        {
            const auto rate = intervalSeconds > 0 ? static_cast<size_t>(intervalData.m_count / intervalSeconds) : 0;
            auto       intervalAverageLatency = intervalData.m_summaryLatency / intervalData.m_count;
            COUT(format("{:<10} {:>10d} {:>10} {:>12}", duration_cast<milliseconds>(interval), intervalData.m_count, intervalAverageLatency, rate));
            intervalAverageLatencies.emplace_back(intervalAverageLatency);
        }
    }
#ifdef _WIN32
    COUT("---------------------------------------------");
#else
    COUT("─────────────────────────────────────────────");
#endif

    if (totalCount)
    {
        const auto totalSeconds = !m_intervals.empty()
                                      ? duration<double>(m_intervals.rbegin()->first + m_groupIntervalDuration).count()
                                      : 0.0;
        const auto averageRate = totalSeconds > 0 ? static_cast<size_t>(totalCount / totalSeconds) : 0;
        ranges::sort(intervalAverageLatencies);
        auto medianLatency = intervalAverageLatencies[intervalAverageLatencies.size() / 2];
        COUT(std::format("{:<10} {:>10} {:>10} {:>12}", "Average", totalCount, totalLatency / totalCount, averageRate));
        COUT(std::format("{:<21} {:>10}", "Median", medianLatency));
    }
    else
    {
        COUT(std::format("{:<10} {:>10} {:>10} {:>12}", "Average", 0, "?", "?"));
    }
    COUT("");
}
