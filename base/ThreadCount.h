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
 * @brief How many send threads to run when the configuration says "auto".
 *
 * Four, flat. The count used to be a logarithm of the physical core count, calibrated on a single
 * eight-core machine; the 2026-10-17 AWS campaign (c5n.4xlarge, 16 vCPU, 8 physical cores) showed
 * that the Point-To-Point over TLS at 100000 connections needs four on the send side to keep up
 * with the receive side, and that this is also what the eight-thread bench answers. Flat, because
 * the two machines agreed: the curve is flat either side of the optimum, and one thread too few
 * costs a few per cent while one too many was measured to cost everything - eight send threads on
 * eight cores collapsed a Fan-Out to eleven seconds of latency and dropped eight per cent of the
 * messages on 2026-09-11. Anyone the four does not suit writes the number instead, and the startup
 * log says what it chose.
 */
[[nodiscard]] inline size_t automaticThreadCount()
{
    return 4;
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
 * @brief Receive threads for "auto": six.
 *
 * Six, not four, since 2026-10-17. TLS decryption happens on the receive path, and a TLS
 * Point-To-Point at 100000 connections put more on it than four threads could carry: on the
 * eight-thread bench four threads gave a 413 ms median at 100K against 1.4 ms at eight, and on the
 * AWS instance four gave 549us against FlashMQ's 254us while six gave 273us - seven per cent behind
 * FlashMQ, and within a run of the eight-thread answer (267us). The count does not have to track the
 * core count: on both machines six was enough, and the extra two buy the TLS headroom that four did
 * not have.
 *
 * Higher than the send side because a receive thread also matches and delivers what it reads when
 * the broker runs alone, so it does more than parse.
 */
[[nodiscard]] inline size_t automaticReceiveThreadCount()
{
    return 6;
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
 * @brief Delivery workers for "auto": four.
 *
 * Four since 2026-10-17, from two. Two was chosen when the send and receive counts were three and
 * four; with them at four and six the delivery stage became the one that held a Fan-Out back, and
 * four workers recovered it (1788us on the bench against FlashMQ's 1807us). Two was tried again on
 * the AWS instance before this default was accepted, and four won there too: 1972us against 2032us.
 * An explicit setting still overrides this.
 */
[[nodiscard]] inline size_t automaticDeliveryThreadCount()
{
    return 4;
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
