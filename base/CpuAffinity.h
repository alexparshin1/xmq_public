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

#include <sptk5/String.h>

namespace xmq {

/**
 * @brief Restricts the process to one hardware thread per physical CPU core.
 *
 * The broker spends most of its time in the kernel, and two hyperthreads on one core contend for
 * the same execution units, L1/L2 and TLB rather than adding throughput. Measured on a 4-core /
 * 8-thread Skylake at 50K messages/second over 100K connections, confining the server to the four
 * physical cores gave 27% less CPU per message, instructions-per-cycle 0.38 -> 0.69, five times
 * fewer scheduler migrations, and cut typical round trip latency from seconds to ~2ms.
 *
 * Cores are read from sysfs topology, keeping the lowest-numbered sibling of each core. Affinity
 * is inherited by threads created afterwards, so this must run before the server starts any.
 */
class CpuAffinity
{
public:
    /**
     * @brief Restrict this process to the CPUs worth running on.
     *
     * Two things can be wrong with letting a process use every CPU it is given, and a machine can
     * have either, both, or neither - so both are checked. Where cores differ in speed, the slow
     * ones are dropped: on a hybrid processor they can be a quarter slower and outside the last
     * level of cache, and the scheduler moves threads onto them according to what else the machine
     * is doing. Where a core carries two hardware threads, one of them is dropped: two threads on
     * one core share its execution units.
     *
     * @param description       Receives what was decided, in words, whether or not anything changed.
     * @return true when the affinity was changed.
     */
    static bool useBestCores(sptk::String& description);

    /**
     * @brief How many physical cores this process may run on, or 0 when that cannot be told.
     *
     * The same sysfs walk the affinity uses, and the same honesty about not guessing: a machine
     * whose topology cannot be read answers 0 rather than a number that might be the count of
     * hardware threads. Callers that need an answer anyway can fall back to
     * std::thread::hardware_concurrency(), knowing what they are getting.
     */
    [[nodiscard]] static size_t physicalCoreCount();
};

} // namespace xmq
