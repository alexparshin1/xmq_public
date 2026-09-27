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

#include "PaceMaker.h"

#include <thread>

using namespace std;
using namespace std::chrono;
using namespace xmq;

PaceMaker::PaceMaker(const size_t eventRate)
    : m_interval(eventRate > 0 ? nanoseconds(seconds(1)) / static_cast<long>(eventRate) : nanoseconds(0))
{
}

bool PaceMaker::next()
{
    steady_clock::time_point deadline;

    {
        // Reserve this call's time slot under the lock. This is the only critical section; it
        // must be serialised because next() is called concurrently from many worker threads and
        // the schedule is a single global stream of ticks.
        const lock_guard lock(m_mutex);
        const auto       now = steady_clock::now();

        if (!m_started)
        {
            // First tick fires immediately and establishes the baseline schedule.
            m_started = true;
            m_nextDeadline = now;
        }
        else if (constexpr auto maxLag = milliseconds(50);
                 now - m_nextDeadline > maxLag)
        {
            // The callers have fallen far behind the schedule (they cannot keep up with the
            // requested rate). Re-base off the freshly sampled clock to avoid an unbounded
            // catch-up burst that would defeat the pacing.
            m_nextDeadline = now;
        }

        deadline = m_nextDeadline;
        // Advance by a fixed interval (drift-free) so the next caller gets the following slot.
        m_nextDeadline += m_interval;
    }

    // Sleep outside the lock so concurrent callers pipeline towards their own slots instead of
    // serialising on the sleep.
    this_thread::sleep_until(deadline);

    return true;
}
