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

#pragma once

#include "client/MqttClient.h"
#include "service/CTestScenario.h"

#include <cstdint>
#include <map>
#include <unordered_map>

namespace xmq {

/**
 * @brief Measures round-trip latency for MQTT messages in microseconds.
 */
class RoundTripLatency
{
public:
    using Clock = std::chrono::high_resolution_clock;
    /// Latency buckets, log-spaced: sixty-four per power of two, so an approximate percentile is
    /// within about 1.6% of the value. Kept per interval and merged for the run's percentiles;
    /// the sums and counts alone cannot answer a percentile question.
    ///
    /// Sparse is the shape for what is written once an interval and read a handful of times at
    /// the end. The two histograms written on every delivery use HistogramCounts below instead:
    /// they were this map, and charging one tree walk and a node allocation per delivered message
    /// measured at 2.48% of all samples on the machine - 35% of the load generator's userspace
    /// CPU, taken by the measurement whose latency it reports.
    struct HistogramCounts
    {
        std::vector<uint64_t> m_counts;  ///< Count per bucket index, sized lazily to the range seen.
        std::vector<uint16_t> m_touched; ///< Bucket indices with a non-zero count, in arrival order.
    };

    /// Bucket indices a dense histogram can hold, with HistogramCounts covering two to the
    /// thirty-second power of microseconds and change. A delivery past this counts in the last
    /// bucket, so a stalled message still shows as "at least its top edge".
    static constexpr uint32_t HistogramBucketLimit = 2048;

    /// The same bucket space, sparse: one key per bucket index a range ever saw.
    using Histogram = std::map<uint32_t, uint64_t>;

    /**
     * @brief Add samples to a dense histogram at an already-computed bucket.
     *
     * The place a HistogramCounts grows: resizing here means every user shares one growth
     * pattern, whatever bucket index it was handed.
     */
    static void countHistogram(HistogramCounts& histogram, const uint32_t bucket, const uint64_t samples = 1)
    {
        if (samples == 0)
        {
            return;
        }
        if (bucket >= histogram.m_counts.size())
        {
            const auto target = std::min<size_t>(HistogramBucketLimit,
                                                 std::max<size_t>(bucket + 1, histogram.m_counts.size() * 2));
            histogram.m_counts.resize(target, 0);
        }
        if (histogram.m_counts[bucket] == 0)
        {
            histogram.m_touched.push_back(static_cast<uint16_t>(bucket));
        }
        histogram.m_counts[bucket] += samples;
    }

    struct Interval
    {
        size_t                    m_count {0};
        std::chrono::microseconds m_summaryLatency {0};
        Histogram                 m_histogram;
    };

    RoundTripLatency() = default;
    RoundTripLatency(const RoundTripLatency& other);

    void start(std::chrono::microseconds groupInterval);

    void stop();

    void sent();

    /**
     * @brief Record the send time of a message by its sequence number.
     *
     * The sequence travels in the payload, so the receive side can pair a delivery with its own
     * send even after losses, duplicates or reordering. FIFO pairing cannot do that: one lost
     * message shifts every later pairing and turns the loss into a constant apparent latency.
     */
    void sent(uint32_t sequence);

    void storeCurrentInterval();

    void received();

    /**
     * @brief Pair a delivery with the send recorded under @p sequence, if there is one.
     *
     * A delivery with no send - a duplicate, a message from another run, or a warm-up message
     * that arrived after the measurement started - is counted as unmatched and changes no
     * percentile.
     */
    void received(uint32_t sequence);

    /// Forget a send that failed, so it cannot be mistaken for a lost delivery.
    void cancel(uint32_t sequence);

    void add(std::chrono::microseconds duration);

    size_t count() const;

    std::chrono::microseconds averageLatency() const;

    /**
     * @brief A percentile of the individual message latencies, or 0 when there are none.
     *
     * Approximate: the histogram has sixty-four buckets per power of two, so the answer is the
     * upper edge of the bucket the rank falls in, within about 1.6% of the true value.
     */
    [[nodiscard]] std::chrono::microseconds percentile(double percent) const;

    /// Deliveries whose sequence number had no recorded send.
    [[nodiscard]] size_t unmatchedReceived() const;

    void print(const std::string& scenarioTitle) const;

    /// The same result, with box-drawing borders, for a terminal. Plain text when redirected.
    void printBoxed(const std::string& scenarioTitle) const;

    static RoundTripLatency combine(const std::vector<RoundTripLatency>& roundTripLatencies)
    {
        RoundTripLatency summaryRoundTripLatency({});
        if (!roundTripLatencies.empty())
        {
            // Every element was start()-ed with the same bucket width (see publish()'s single
            // groupInterval computation), so any one of them gives the summary object the width
            // it needs for print()'s rate column - a default-constructed object has 0 here,
            // which print() would otherwise divide by.
            summaryRoundTripLatency.m_groupIntervalDuration = roundTripLatencies.front().m_groupIntervalDuration;
        }
        for (const auto& roundTripLatency: roundTripLatencies)
        {
            summaryRoundTripLatency.m_unmatchedReceived += roundTripLatency.m_unmatchedReceived;
            for (const auto& bucket: roundTripLatency.m_totalHistogram.m_touched)
            {
                countHistogram(summaryRoundTripLatency.m_totalHistogram, bucket,
                               roundTripLatency.m_totalHistogram.m_counts[bucket]);
            }
            for (const auto& [intervalTime, intervalData]: roundTripLatency.m_intervals)
            {
                auto& [count, summaryLatency, histogram] = summaryRoundTripLatency.m_intervals[intervalTime];
                summaryLatency += intervalData.m_summaryLatency;
                count += intervalData.m_count;
                for (const auto& [bucket, samples]: intervalData.m_histogram)
                {
                    histogram[bucket] += samples;
                }
            }
        }
        return summaryRoundTripLatency;
    }

private:
    mutable std::mutex                                 m_mutex;
    sptk::SynchronizedQueue<std::chrono::microseconds> m_started;
    std::chrono::microseconds                          m_summaryLatency {0};
    size_t                                             m_count {0};
    HistogramCounts                                    m_histogram;    ///< The interval being collected.
    HistogramCounts                                    m_totalHistogram; ///< The whole measured run.
    std::unordered_map<uint32_t, std::chrono::microseconds> m_startedBySequence;
    size_t                                             m_unmatchedReceived {0};
    std::chrono::microseconds                          m_groupIntervalDuration {0};
    std::map<std::chrono::microseconds, Interval>      m_intervals;
    Clock::time_point                                  m_startTime;
    Clock::time_point                                  m_nextIntervalTime;
};


} // namespace xmq
