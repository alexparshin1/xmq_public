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

#include <chrono>
#include <cstddef>
#include <mutex>

namespace xmq {

/**
 * @brief Rate limiter for load-generating loops.
 *
 * Paces calls to next() so that they occur at a steady eventRate per second.
 * The schedule is drift-free: each tick deadline is advanced by a fixed interval
 * rather than re-based on the wake-up time, so accumulated scheduler jitter does
 * not slow the average rate.
 *
 * next() is thread-safe: it may be called concurrently from any number of worker
 * threads and still emits a single global stream of ticks at eventRate per second.
 * Each call reserves the next time slot under a short lock, then sleeps to that slot
 * outside the lock, so threads do pipeline rather than serialize on the sleep.
 */
class PaceMaker
{
public:
    /**
     * @brief Constructor.
     * @param eventRate Target number of events (next() calls) per second.
     */
    explicit PaceMaker(size_t eventRate);

    /**
     * @brief Block until the next scheduled tick, then return.
     * @return Always true (kept for call-site compatibility).
     */
    bool next();

    /**
     * @brief Get the interval between ticks.
     * @return Interval between ticks.
     */
    std::chrono::nanoseconds interval() const
    {
        return m_interval;
    }

private:
    std::mutex                            m_mutex;           ///< Guards the slot-reservation state below.
    std::chrono::nanoseconds              m_interval;        ///< Spacing between consecutive ticks.
    std::chrono::steady_clock::time_point m_nextDeadline;    ///< Deadline of the next tick to hand out.
    bool                                  m_started {false}; ///< Whether the baseline has been set.
};

} // namespace xmq
