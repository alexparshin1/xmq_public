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

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <ranges>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace std;
using namespace chrono;
using namespace sptk;
using namespace xmq;

namespace {

bool stdoutIsTerminal()
{
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

std::string repeated(std::string_view unit, const size_t count)
{
    std::string result;
    result.reserve(unit.size() * count);
    for (size_t i = 0; i < count; ++i)
    {
        result += unit;
    }
    return result;
}

constexpr auto bucketsPerPower = uint32_t(64);

uint32_t histogramBucket(const int64_t microseconds)
{
    const auto value = static_cast<uint64_t>(max<int64_t>(microseconds, 1));
    const auto power = bit_width(value) - 1; // floor(log2(value))
    const auto base = uint64_t(1) << power;
    return power * bucketsPerPower + static_cast<uint32_t>(((value - base) * bucketsPerPower) / base);
}

microseconds histogramCeiling(const uint32_t bucket)
{
    const auto power = bucket / bucketsPerPower;
    const auto fraction = bucket % bucketsPerPower;
    const auto base = uint64_t(1) << power;
    return microseconds(base + (base * (fraction + 1)) / bucketsPerPower);
}

void addToHistogram(RoundTripLatency::Histogram& histogram, const microseconds latency)
{
    ++histogram[histogramBucket(latency.count())];
}

microseconds percentileOf(const RoundTripLatency::Histogram& histogram, const double percent)
{
    uint64_t total = 0;
    for (const auto& samples: histogram | views::values)
    {
        total += samples;
    }
    if (total == 0)
    {
        return {};
    }

    const auto rank = static_cast<uint64_t>(
        clamp(ceil(percent / 100.0 * static_cast<double>(total)), 1.0, static_cast<double>(total)));

    uint64_t seen = 0;
    for (const auto& [bucket, samples]: histogram)
    {
        seen += samples;
        if (seen >= rank)
        {
            return histogramCeiling(bucket);
        }
    }
    return histogramCeiling(histogram.rbegin()->first);
}

} // namespace

RoundTripLatency::RoundTripLatency(const RoundTripLatency& other)
{
    const std::scoped_lock lock(other.m_mutex);
    m_groupIntervalDuration = other.m_groupIntervalDuration;
    m_startTime = other.m_startTime;
    m_nextIntervalTime = other.m_nextIntervalTime;
    m_intervals = other.m_intervals;
    m_summaryLatency = other.m_summaryLatency;
    m_count = other.m_count;
    m_histogram = other.m_histogram;
    m_totalHistogram = other.m_totalHistogram;
    m_startedBySequence = other.m_startedBySequence;
    m_unmatchedReceived = other.m_unmatchedReceived;
}

void RoundTripLatency::start(const microseconds groupInterval)
{
    m_groupIntervalDuration = groupInterval;

    m_startTime = Clock::now();
    m_nextIntervalTime = m_startTime + m_groupIntervalDuration;

    m_summaryLatency = microseconds(0);
    m_count = 0;
    m_histogram.clear();
    m_totalHistogram.clear();
    m_startedBySequence.clear();
    m_unmatchedReceived = 0;
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

void RoundTripLatency::sent(const uint32_t sequence)
{
    const std::scoped_lock lock(m_mutex);
    m_startedBySequence[sequence] = duration_cast<microseconds>(Clock::now().time_since_epoch());
}

void RoundTripLatency::cancel(const uint32_t sequence)
{
    const std::scoped_lock lock(m_mutex);
    m_startedBySequence.erase(sequence);
}

void RoundTripLatency::storeCurrentInterval()
{
    if (m_count)
    {
        // Key the bucket by the wall-clock start of the interval being closed, so buckets stay
        // aligned across objects and across delivery gaps (m_intervals.size() would drift after
        // an interval with no samples).
        const auto interval = duration_cast<microseconds>(m_nextIntervalTime - m_groupIntervalDuration - m_startTime);
        auto& [count, summaryLatency, histogram] = m_intervals[interval];
        count += m_count;
        summaryLatency += m_summaryLatency;
        for (const auto& [bucket, samples]: m_histogram)
        {
            histogram[bucket] += samples;
        }
        m_summaryLatency = {};
        m_count = 0;
        m_histogram.clear();
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
        addToHistogram(m_histogram, latency);
        addToHistogram(m_totalHistogram, latency);
        ++m_count;
    }
}

void RoundTripLatency::received(const uint32_t sequence)
{
    const std::scoped_lock lock(m_mutex);

    const auto started = m_startedBySequence.find(sequence);
    if (started == m_startedBySequence.end())
    {
        ++m_unmatchedReceived;
        return;
    }

    const auto now = Clock::now();
    const auto latency = duration_cast<microseconds>(now.time_since_epoch()) - started->second;
    m_startedBySequence.erase(started);

    while (m_groupIntervalDuration.count() > 0 && now > m_nextIntervalTime)
    {
        storeCurrentInterval();
    }

    m_summaryLatency += latency;
    addToHistogram(m_histogram, latency);
    addToHistogram(m_totalHistogram, latency);
    ++m_count;
}

void RoundTripLatency::add(const microseconds duration)
{
    const std::scoped_lock lock(m_mutex);
    m_summaryLatency += duration;
    addToHistogram(m_histogram, duration);
    addToHistogram(m_totalHistogram, duration);
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
    for (const auto& [_count, _summaryLatency, _histogram]: m_intervals | std::views::values)
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

microseconds RoundTripLatency::percentile(const double percent) const
{
    const std::scoped_lock lock(m_mutex);
    return percentileOf(m_totalHistogram, percent);
}

size_t RoundTripLatency::unmatchedReceived() const
{
    const std::scoped_lock lock(m_mutex);
    return m_unmatchedReceived;
}

void RoundTripLatency::print(const string& scenarioTitle) const
{
    if (stdoutIsTerminal())
    {
        printBoxed(scenarioTitle);
        return;
    }

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
        COUT("");
        COUT("Percentiles");
        COUT(std::format("{:<6} {}", "p50", percentileOf(m_totalHistogram, 50)));
        COUT(std::format("{:<6} {}", "p90", percentileOf(m_totalHistogram, 90)));
        COUT(std::format("{:<6} {}", "p95", percentileOf(m_totalHistogram, 95)));
        COUT(std::format("{:<6} {}", "p99", percentileOf(m_totalHistogram, 99)));
        // Below a thousand samples the 99.9th percentile is the maximum: one slow delivery is
        // enough to set it, and calling that a percentile says more about the sample count than
        // about the tail. Print it only when enough deliveries make it a percentile, and always
        // print the maximum so the worst delivery is not hidden.
        if (totalCount >= 1000)
        {
            COUT(std::format("{:<6} {}", "p99.9", percentileOf(m_totalHistogram, 99.9)));
        }
        COUT(std::format("{:<6} {}", "max", percentileOf(m_totalHistogram, 100)));
        if (m_unmatchedReceived > 0)
        {
            COUT("Unmatched deliveries: " << m_unmatchedReceived
                                         << " had a sequence number with no recorded send (duplicate, warm-up or another run)");
        }
    }
    else
    {
        COUT(std::format("{:<10} {:>10} {:>10} {:>12}", "Average", 0, "?", "?"));
    }
    COUT("");
}

void RoundTripLatency::printBoxed(const string& scenarioTitle) const
{
    const std::scoped_lock lock(m_mutex);
    COUT("Scenario: " << scenarioTitle);

#ifdef _WIN32
    constexpr auto v = "|";
    constexpr auto h = "-";
    constexpr auto tl = "+", tm = "+", tr = "+", ml = "+", mm = "+", mr = "+", bl = "+", bm = "+", br = "+";
#else
    constexpr auto v = "│";
    constexpr auto h = "─";
    constexpr auto tl = "┌", tm = "┬", tr = "┐", ml = "├", mm = "┼", mr = "┤", bl = "└", bm = "┴", br = "┘";
#endif

    const auto line4 = [&](const char* left, const char* middle, const char* right)
    {
        COUT(left << repeated(h, 10) << middle << repeated(h, 10) << middle << repeated(h, 10) << middle
                 << repeated(h, 12) << right);
    };
    const auto row4 = [&](const string_view a, const string_view b, const string_view c, const string_view d)
    {
        COUT(v << std::format("{:<10}", a) << v << std::format("{:>10}", b) << v
              << std::format("{:>10}", c) << v << std::format("{:>12}", d) << v);
    };
    const auto line2 = [&](const char* left, const char* middle, const char* right)
    {
        COUT(left << repeated(h, 7) << middle << repeated(h, 12) << right);
    };
    const auto row2 = [&](const string_view label, const string_view value)
    {
        COUT(v << std::format("{:<7}", label) << v << std::format("{:>12}", value) << v);
    };

    line4(tl, tm, tr);
    row4("Interval", "Count", "Latency", "Rate/s");
    line4(ml, mm, mr);

    const auto           intervalSeconds = duration<double>(m_groupIntervalDuration).count();
    size_t               totalCount = 0;
    microseconds         totalLatency {0};
    vector<microseconds> intervalAverageLatencies;

    for (const auto& [interval, intervalData]: m_intervals)
    {
        totalCount += intervalData.m_count;
        totalLatency += intervalData.m_summaryLatency;
        const auto intervalText = std::format("{}", duration_cast<milliseconds>(interval));
        if (intervalData.m_count == 0)
        {
            row4(intervalText, "0", "?", "?");
            continue;
        }
        const auto rate = intervalSeconds > 0 ? static_cast<size_t>(intervalData.m_count / intervalSeconds) : 0;
        const auto averageLatency = intervalData.m_summaryLatency / intervalData.m_count;
        row4(intervalText, std::to_string(intervalData.m_count), std::format("{}", averageLatency),
             std::to_string(rate));
        intervalAverageLatencies.emplace_back(averageLatency);
    }

    line4(ml, mm, mr);

    if (totalCount)
    {
        const auto totalSeconds = !m_intervals.empty()
                                      ? duration<double>(m_intervals.rbegin()->first + m_groupIntervalDuration).count()
                                      : 0.0;
        const auto averageRate = totalSeconds > 0 ? static_cast<size_t>(totalCount / totalSeconds) : 0;
        ranges::sort(intervalAverageLatencies);
        const auto medianLatency = intervalAverageLatencies[intervalAverageLatencies.size() / 2];
        row4("Average", std::to_string(totalCount), std::format("{}", totalLatency / totalCount),
             std::to_string(averageRate));
        row4("Median", "", std::format("{}", medianLatency), "");
    }
    else
    {
        row4("Average", "0", "?", "?");
    }

    line4(bl, bm, br);

    COUT("");
    COUT("Percentiles");
    line2(tl, tm, tr);
    row2("Metric", "Latency");
    line2(ml, mm, mr);
    row2("p50", std::format("{}", percentileOf(m_totalHistogram, 50)));
    row2("p90", std::format("{}", percentileOf(m_totalHistogram, 90)));
    row2("p95", std::format("{}", percentileOf(m_totalHistogram, 95)));
    row2("p99", std::format("{}", percentileOf(m_totalHistogram, 99)));
    if (totalCount >= 1000)
    {
        row2("p99.9", std::format("{}", percentileOf(m_totalHistogram, 99.9)));
    }
    row2("max", std::format("{}", percentileOf(m_totalHistogram, 100)));
    line2(bl, bm, br);

    if (m_unmatchedReceived > 0)
    {
        COUT("Unmatched deliveries: " << m_unmatchedReceived
                                     << " had a sequence number with no recorded send (duplicate, warm-up or another run)");
    }
    COUT("");
}
