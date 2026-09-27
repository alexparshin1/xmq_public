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

#include "xmq.h"

#include <cstdlib>

namespace xmq {

// Declared, not included: IMessageProperties.h includes this header, and the latency trace travels
// inside the properties as a user property - so the two know of each other by name only.
class IMessageProperties;
constexpr uint32_t LatencyTraceSignature = 0x5115;

enum class LatencyPhase : uint8_t
{
    ClientSend,
    ClientConnect,
    ClientConnected,
    ClientWireOut,
    ServerEventReady,
    ServerWireIn,
    ServerBeforeDecode,
    ServerDecode,
    ServerHandler,
    ServerDelivery,
    ServerWireOut,
    ClientWireIn,
    ClientBeforeDecode,
    ClientDecode,
    ClientReceive,
    ItemCount
};

constexpr uint32_t LatencyPhaseItemCount = static_cast<uint32_t>(LatencyPhase::ItemCount);

class MessageProperties;

class XMQ_EXPORT LatencyTrace
{
public:
    uint16_t                                    m_signature {LatencyTraceSignature};
    std::array<uint64_t, LatencyPhaseItemCount> m_trace {};

    void clear()
    {
        m_trace = {};
    }

    /**
     * @brief Whether this process should take latency timestamps outside the message trace.
     *
     * Most phases cost nothing when tracing is off, because they are stamped through a message's
     * properties and there is no trace there to write into. The reactor's readiness timestamp has
     * no message yet, so it needs a gate of its own - set XMQ_LATENCY_TRACE in the broker's
     * environment, the same variable xmq_scn uses to attach traces in the first place.
     */
    static bool tracingEnabled()
    {
        static const bool enabled = std::getenv("XMQ_LATENCY_TRACE") != nullptr;
        return enabled;
    }

    static uint64_t now()
    {
        // Wall clock (system_clock), not steady_clock: phases here get snapped by the publisher,
        // the server, and the subscriber, which in any real deployment are three separate
        // processes - often on three separate machines. steady_clock's epoch is arbitrary and
        // per-machine (typically boot time), so a diff between a steady_clock snap taken on the
        // client and one taken on the server is meaningless - not close, not noisy, just an
        // unrelated pair of large numbers producing nonsensical (even multi-day, via unsigned
        // underflow) results whenever a phase pair crosses a process boundary. system_clock is
        // wall-clock and NTP-disciplined, so cross-host diffs are meaningful up to clock skew
        // between hosts (typically sub-ms to a few ms on a synced LAN) - not perfectly accurate,
        // but comparable, unlike steady_clock across hosts.
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    uint64_t get(const LatencyPhase phase) const
    {
        return m_trace[static_cast<uint8_t>(phase)];
    }

    void snap(const LatencyPhase phase)
    {
        m_trace[static_cast<uint8_t>(phase)] = now();
    }

    void set(const LatencyPhase phase, const uint64_t valueMcs)
    {
        m_trace[static_cast<uint8_t>(phase)] = valueMcs;
    }
};

class XMQ_EXPORT LatencyTraceTotal : public LatencyTrace
{
public:
    size_t m_count {0};
    size_t m_rejectedCount {0};
    size_t m_unstampedCount {0}; ///< Samples the broker did not stamp with the reactor's time; see add().

    LatencyTraceTotal() = default;

    LatencyTraceTotal(const LatencyTraceTotal& other);

    LatencyTraceTotal(LatencyTraceTotal&& other) noexcept;

    LatencyTraceTotal& operator=(const LatencyTraceTotal& other);

    LatencyTraceTotal& operator=(LatencyTraceTotal&& other) noexcept;

    /**
     * @brief Fold one completed trace into the running totals.
     *
     * Rejects (does not add) any sample whose set phases aren't monotonically non-decreasing.
     * A correctly-captured trace's phases happen in strict causal order; a shared
     * MessageProperties object that gets re-stamped by a second send of the same message (e.g.
     * QoS1's MessageQueue::rescheduleMessagesWaitingForAck resending after a lost ack) can leave
     * one phase with a later wall-clock value than a phase that should follow it. Summing such a
     * sample would corrupt every phase-pair average that spans the bad field, silently, for the
     * life of this total - so the whole sample is discarded instead. Unset phases (value 0, e.g.
     * ClientConnect/ClientConnected on a publish-only trace) are skipped when checking order.
     *
     * The order is checked *within each clock domain* and not across them. Client and server
     * stamp their phases from their own wall clocks, and two hosts agree only to within their
     * time synchronisation - a millisecond is normal, which is several times the whole
     * measurement. Comparing a client timestamp with a server one therefore says nothing about
     * causality, and checking it rejected 3992 of 4000 samples in a bench run that had nothing
     * wrong with it. Phase pairs spanning the boundary are meaningless for the same reason; the
     * within-server and within-client pairs are exact.
     */
    void add(const LatencyTrace* latency);

    uint64_t diff(LatencyPhase from, LatencyPhase to) const;

    uint64_t duration(LatencyPhase phase) const;

    /**
     * @brief Print a breakdown of the message path, phase by phase, averaged over every
     * sample added so far. Publish-message phases only (ClientConnect/ClientConnected are
     * connect-only and are skipped).
     * @param scenarioTitle     Title to print above the breakdown.
     */
    void print(const std::string& scenarioTitle) const;

private:
    mutable std::mutex m_mutex;
};

class XMQ_EXPORT Latency
{
public:
    static void SNAP_LATENCY(const std::shared_ptr<IMessageProperties>& messageProperties, LatencyPhase phase, LatencyTraceTotal* latencyTraceTotal = nullptr);
    static void SET_LATENCY(const std::shared_ptr<IMessageProperties>& messageProperties, LatencyPhase phase, uint64_t valueMcs);
    static void COPY_LATENCY(const std::shared_ptr<IMessageProperties>& srcMessageProperties, const std::shared_ptr<IMessageProperties>& dstMessageProperties);
};
} // namespace xmq
