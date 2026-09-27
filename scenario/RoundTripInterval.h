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

namespace xmq {

/**
 * @brief Measures round-trip latency for MQTT messages in microseconds.
 */
class RoundTripLatency
{
public:
    using Clock = std::chrono::high_resolution_clock;
    struct Interval
    {
        size_t                    m_count {0};
        std::chrono::microseconds m_summaryLatency {0};
    };

    RoundTripLatency() = default;
    RoundTripLatency(const RoundTripLatency& other);

    void start(std::chrono::microseconds groupInterval);

    void stop();

    void sent();

    void storeCurrentInterval();

    void received();

    void add(std::chrono::microseconds duration);

    size_t count() const;

    std::chrono::microseconds averageLatency() const;

    void print(const std::string& scenarioTitle) const;

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
            for (const auto& [intervalTime, intervalData]: roundTripLatency.m_intervals)
            {
                auto& [count, summaryLatency] = summaryRoundTripLatency.m_intervals[intervalTime];
                summaryLatency += intervalData.m_summaryLatency;
                count += intervalData.m_count;
            }
        }
        return summaryRoundTripLatency;
    }

private:
    mutable std::mutex                                 m_mutex;
    sptk::SynchronizedQueue<std::chrono::microseconds> m_started;
    std::chrono::microseconds                          m_summaryLatency {0};
    size_t                                             m_count {0};
    std::chrono::microseconds                          m_groupIntervalDuration {0};
    std::map<std::chrono::microseconds, Interval>      m_intervals;
    Clock::time_point                                  m_startTime;
    Clock::time_point                                  m_nextIntervalTime;
};


} // namespace xmq
