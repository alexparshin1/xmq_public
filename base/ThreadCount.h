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

#include "base/CpuAffinity.h"

#include <algorithm>
#include <bit>
#include <sptk5/String.h>
#include <thread>

namespace xmq {

/**
 * @brief How many send or receive threads to run when the configuration says "auto".
 *
 * Logarithmic in the core count, not proportional to it: these threads do not divide the work so
 * much as hand it to one another, and every one added is another hand-off and another contender
 * for the same structures. Measured on AWS 2026-09-11, the difference is not a matter of tuning -
 * a 16-vCPU instance configured with eight send threads delivered a Fan-Out at **11 seconds** of
 * latency and dropped 8% of the messages, where two threads delivered the same load at 1788us with
 * nothing lost. The run-to-run spread fell from 27% to 1.1% at the same time.
 *
 * One per doubling of the physical cores: 4 cores answer 2, 8 answer 3, 16 answer 4. Measured on
 * the 8-core instance above, three interleaved runs a side: 3 threads 1756us, 2 threads 1791us,
 * 4 threads 1886us, 8 threads the collapse described. The curve is flat to the left of its optimum
 * and a cliff to the right, so where the answer has to be rounded it is rounded down: one thread
 * too few costs a few per cent, one too many can cost everything.
 *
 * Calibrated at exactly one point - the eight-core instance above - and floored at the other end.
 * Everything larger is extrapolation, and it is deliberately the safe kind: the collapse found was
 * eight threads on eight cores, one per core, and a logarithm walks away from that line as the
 * machine grows rather than towards it. Sixteen cores answer 4, not 16. The formula can therefore
 * be a little low on a big machine, which costs a few per cent, and cannot be the other thing,
 * which costs messages. Anyone it does not suit can write the number instead, and the startup log
 * says what it chose.
 *
 * Never fewer than three, whatever the arithmetic says. On a four-core machine, where the logarithm
 * asks for two, two is both slower and far less steady than three: scored by interval median, three
 * runs a side gave 2126us with a 69% spread at two threads against 2072us with 3% at three, and the
 * spread is the point - at two threads whole minutes of a run leave the 2ms band for 3.5-4.3ms and
 * come back. Four is no better than three there (2064us), so three costs nothing and buys the
 * stability. A single receive thread loses messages outright, which is the same argument one step
 * further down.
 *
 * The receive side answers the same number, and was measured separately on 2026-09-13. Fan-Out
 * cannot say anything about it - five publishers feed it - so the sweep ran on Fan-In, where 50000
 * publishers send into 500 subscribers and the receive path is what the run is made of. Scored by
 * interval median on a four-core bench: three threads 194 and 196us, four 195, 197 and 197us, six
 * 196, 198 and 199us. Flat. Both ends misbehave in the same way - two threads gave 194 and 225us,
 * eight gave 200 and 224us - which is the same shape as the send side and the same reason for the
 * floor. The template used to ship eight of each; on the receive side that is not an optimum but a
 * number somebody wrote down.
 *
 * Where the topology cannot be read the hardware threads are counted instead and one is subtracted,
 * which gives the same answer on everything with two threads to a core.
 */
[[nodiscard]] inline size_t automaticThreadCount()
{
    constexpr int fewestThreads = 3;

    // bit_width(n) is floor(log2(n)) + 1 for any n > 0, and it returns int - so the subtraction is
    // done in a signed type, where a single-core machine gives a negative the clamp can deal with
    // rather than an enormous size_t.
    if (const auto physicalCores = CpuAffinity::physicalCoreCount();
        physicalCores > 0)
    {
        return static_cast<size_t>(std::max(fewestThreads, std::bit_width(physicalCores) - 1));
    }

    const auto hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0)
    {
        return fewestThreads; // Nothing to go on at all
    }
    return static_cast<size_t>(
        std::max(fewestThreads, std::bit_width(static_cast<size_t>(hardwareThreads)) - 2));
}

/**
 * @brief The thread count a configured value asks for: a number, or "auto", or 0 meaning auto.
 *
 * "auto" and 0 mean the same thing, because a setting that used to be a number should be able to
 * say "decide for me" without anyone having to remember which spelling this file wants.
 *
 * @param configured    The configured value, "auto" or a decimal number.
 */
[[nodiscard]] inline bool isAutomaticThreadCount(const sptk::String& configured)
{
    const auto text = sptk::String(configured).trim().toLowerCase();
    return text.empty() || text == "auto" || text == "0";
}

/**
 * @brief Receive threads for "auto": no fewer than four.
 *
 * A receive thread also matches and delivers what it reads when the broker runs alone, so it does more
 * than parse, and fewer threads leave that stage the narrowest one.
 */
[[nodiscard]] inline size_t automaticReceiveThreadCount()
{
    return std::max<size_t>(4, automaticThreadCount());
}

[[nodiscard]] inline size_t resolveReceiveThreadCount(const sptk::String& configured)
{
    if (isAutomaticThreadCount(configured))
    {
        return automaticReceiveThreadCount();
    }
    return static_cast<size_t>(sptk::String(configured).trim().toInt());
}

[[nodiscard]] inline size_t resolveThreadCount(const sptk::String& configured)
{
    if (isAutomaticThreadCount(configured))
    {
        return automaticThreadCount();
    }
    return static_cast<size_t>(sptk::String(configured).trim().toInt());
}

/**
 * @brief Use two delivery workers when the setting is "auto".
 *
 * On our load-test bench, two workers reduced P2P latency relative to the former default of four without
 * showing a regression in the other short scenario runs. An explicit setting still overrides this.
 */
[[nodiscard]] inline size_t automaticDeliveryThreadCount()
{
    return 2;
}

[[nodiscard]] inline size_t resolveDeliveryThreadCount(const sptk::String& configured)
{
    if (isAutomaticThreadCount(configured))
    {
        return automaticDeliveryThreadCount();
    }
    return static_cast<size_t>(sptk::String(configured).trim().toInt());
}

} // namespace xmq
