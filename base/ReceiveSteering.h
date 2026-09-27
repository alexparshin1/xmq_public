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

// Not for a type used below, but for the linker. sptk5::String derives from std::string and is
// __declspec(dllimport) here, which makes MSVC take std::basic_string<char>'s members from
// spwsdl5.dll in every translation unit that sees it. A unit that does not emits its own
// copies instead, and the two collide: LNK2005, ~40 times, on std::string's members. Every
// other header in base/ pulls SPTK in for a type it needs; this one has to ask on purpose.
#include <sptk5/String.h>

#include <string>
#include <vector>

namespace xmq {

/**
 * @brief A network interface that carries broker traffic through one receive queue, unsteered.
 */
struct ReceiveSteeringGap
{
    std::string interfaceName;     ///< Interface name, as in /sys/class/net.
    size_t      receiveQueues {0}; ///< Receive queues the interface exposes.
};

/**
 * @brief Finds interfaces that would want Linux receive packet steering (RPS) and do not have it.
 *
 * A network card with a single receive queue delivers every packet to one CPU, and the kernel then
 * runs the whole IP and TCP receive path on that CPU. At a few hundred thousand packets a second
 * that CPU sits at 100% softirq while the others idle, the card's ring overflows, and clients
 * retransmit into backoff until the broker closes them for silence. Measured on 2026-09-13 on an
 * e1000e: 255 891 frames a second dropped, and a 100k msg/s scenario ending in "Not connected"
 * with every session carrying traffic. RPS spreads that work over other CPUs in software and took
 * the drops to zero.
 *
 * Nothing here changes a setting. The broker only reads sysfs and says what it found: whether to
 * steer, and onto which CPUs, is the administrator's decision.
 *
 * Linux only. FreeBSD has netisr and Windows has RSS, but neither is read here.
 */
class ReceiveSteering
{
public:
    /**
     * @brief Interfaces serving these bind addresses that have one receive queue and no RPS.
     * @param bindAddresses  Listener bind addresses. Empty, or "0.0.0.0", means every interface.
     * @param usableCpus     CPUs the broker may run on. Two or fewer leave nowhere to steer to.
     * @param sysfsNetRoot   Where interfaces are described; a test points this at a fake tree.
     * @return Interfaces that would benefit, in name order. Empty on other platforms.
     */
    [[nodiscard]] static std::vector<ReceiveSteeringGap> findGaps(const std::vector<std::string>& bindAddresses,
                                                                  size_t usableCpus,
                                                                  const std::string& sysfsNetRoot = "/sys/class/net");

    /**
     * @brief Whether one interface would benefit from RPS.
     *
     * True for a physical interface that is up, exposes exactly one receive queue, and has an
     * empty steering mask - on a machine with more than two usable CPUs. Loopback, bridges,
     * container veths and tunnels have no "device" link and are never reported: their receive path
     * is not a card's interrupt.
     *
     * @param interfaceDirectory  The interface's directory, e.g. /sys/class/net/eth0.
     * @param usableCpus          CPUs the broker may run on.
     * @param receiveQueues       Receives the number of receive queues found.
     */
    [[nodiscard]] static bool needsSteering(const std::string& interfaceDirectory, size_t usableCpus,
                                            size_t& receiveQueues);

    /**
     * @brief Names of the local interfaces holding these IPv4 addresses.
     * @param bindAddresses  Empty, or containing "0.0.0.0", selects every interface with an IPv4 address.
     */
    [[nodiscard]] static std::vector<std::string> interfacesForAddresses(const std::vector<std::string>& bindAddresses);
};

} // namespace xmq
