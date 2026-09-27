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

#include "LatencyTrace.h"
#include "MessageProperties.h"

#include <sptk5/Printer.h>

#include <array>
#include <format>
#include <utility>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

LatencyTraceTotal::LatencyTraceTotal(const LatencyTraceTotal& other)
    : LatencyTrace(other)
    , m_count(other.m_count)
    , m_rejectedCount(other.m_rejectedCount)
    , m_unstampedCount(other.m_unstampedCount)
{
}

LatencyTraceTotal::LatencyTraceTotal(LatencyTraceTotal&& other) noexcept
{
    const std::scoped_lock lock(m_mutex, other.m_mutex);
    m_count = other.m_count;
    m_rejectedCount = other.m_rejectedCount;
    m_unstampedCount = other.m_unstampedCount;
    *static_cast<LatencyTrace*>(this) = std::move(*static_cast<LatencyTrace*>(&other));
}

LatencyTraceTotal& LatencyTraceTotal::operator=(const LatencyTraceTotal& other)
{
    if (this != &other)
    {
        const std::scoped_lock lock(m_mutex, other.m_mutex);
        m_count = other.m_count;
        m_rejectedCount = other.m_rejectedCount;
        m_unstampedCount = other.m_unstampedCount;
        *static_cast<LatencyTrace*>(this) = *static_cast<const LatencyTrace*>(&other);
    }
    return *this;
}

LatencyTraceTotal& LatencyTraceTotal::operator=(LatencyTraceTotal&& other) noexcept
{
    if (this != &other)
    {
        const std::scoped_lock lock(m_mutex, other.m_mutex);
        m_count = other.m_count;
        m_rejectedCount = other.m_rejectedCount;
        m_unstampedCount = other.m_unstampedCount;
        *static_cast<LatencyTrace*>(this) = std::move(*static_cast<LatencyTrace*>(&other));
    }
    return *this;
}

void LatencyTraceTotal::add(const LatencyTrace* latency)
{
    const std::scoped_lock lock(m_mutex);

    // Reject the whole sample if its set (nonzero) phases aren't monotonically non-decreasing
    // within a clock domain - see the class-level comment on add() for why the check stops at
    // each domain boundary, and why a valid trace can still fail it.
    static constexpr std::array<std::pair<LatencyPhase, LatencyPhase>, 3> clockDomains {{
        {LatencyPhase::ClientSend, LatencyPhase::ClientWireOut},   // publisher's clock
        {LatencyPhase::ServerEventReady, LatencyPhase::ServerWireOut}, // broker's clock
        {LatencyPhase::ClientWireIn, LatencyPhase::ClientReceive}  // subscriber's clock
    }};

    for (const auto& [firstPhase, lastPhase]: clockDomains)
    {
        uint64_t previousTimestamp = 0;
        for (int i = static_cast<uint8_t>(firstPhase); i <= static_cast<uint8_t>(lastPhase); i++)
        {
            const auto timestamp = latency->m_trace[i];
            if (timestamp == 0)
            {
                continue;
            }
            if (timestamp < previousTimestamp)
            {
                ++m_rejectedCount;
                return;
            }
            previousTimestamp = timestamp;
        }
    }

    // The broker stamps ServerEventReady only when XMQ_LATENCY_TRACE is set in its own environment:
    // the stamp is taken before the packet is read, when nothing yet says the message carries a trace,
    // so the broker needs a switch of its own. Without it every other phase is still measured, and
    // the sample must not be thrown away for the sake of the one that is not.
    //
    // The totals are sums of absolute timestamps, and a phase is the difference of two of them, so a
    // sample that lacks one stamp cannot simply add zero: it would put some fifty years into that
    // phase. It is counted as having had no wait for the reactor instead - its stamp is taken to be
    // the time the packet was read - and counted in m_unstampedCount, so that print() can say which
    // figures are not measured (all samples unstamped) or slightly optimistic (a few of them).
    const auto readyIndex = static_cast<uint8_t>(LatencyPhase::ServerEventReady);
    const auto wireInIndex = static_cast<uint8_t>(LatencyPhase::ServerWireIn);
    const auto unstamped = latency->m_trace[readyIndex] == 0 && latency->m_trace[wireInIndex] != 0;

    ++m_count;
    if (unstamped)
    {
        ++m_unstampedCount;
    }
    for (int i = static_cast<uint8_t>(LatencyPhase::ClientSend); i <= static_cast<uint8_t>(LatencyPhase::ClientReceive); i++)
    {
        m_trace[i] += (unstamped && i == readyIndex) ? latency->m_trace[wireInIndex] : latency->m_trace[i];
    }
}

uint64_t LatencyTraceTotal::diff(const LatencyPhase from, const LatencyPhase to) const
{
    const std::scoped_lock lock(m_mutex);
    if (m_count == 0)
    {
        return 0;
    }
    const auto totalDiffMcs = m_trace[static_cast<uint8_t>(to)] - m_trace[static_cast<uint8_t>(from)];
    const auto averageDiffMcs = totalDiffMcs / m_count;
    return averageDiffMcs;
}

uint64_t LatencyTraceTotal::duration(LatencyPhase phase) const
{
    const auto phaseIndex = static_cast<uint8_t>(phase);
    if (phaseIndex == 0)
    {
        return 0;
    }

    const std::scoped_lock lock(m_mutex);
    if (m_count == 0)
    {
        return 0;
    }
    const auto totalDiffMcs = m_trace[phaseIndex] - m_trace[phaseIndex - 1];
    const auto averageDiffMcs = totalDiffMcs / m_count;
    return averageDiffMcs;
}

void LatencyTraceTotal::print(const string& scenarioTitle) const
{
    using enum LatencyPhase;
    // Publish-message phases only, in wire order; ClientConnect/ClientConnected belong to the
    // CONNECT/CONNACK path and are never set on a publish trace, so they're skipped here.
    static const vector<pair<string, pair<LatencyPhase, LatencyPhase>>> segments = {
        {"Publish accepted -> client wire-out", {ClientSend, ClientWireOut}},
        {"Client wire-out -> server wire-in", {ClientWireOut, ServerEventReady}},
        {"Reactor ready -> packet read", {ServerEventReady, ServerWireIn}},
        {"Packet read -> decode start", {ServerWireIn, ServerBeforeDecode}},
        {"Server decode", {ServerBeforeDecode, ServerDecode}},
        {"Server decode -> handler", {ServerDecode, ServerHandler}},
        {"Server handler -> delivery", {ServerHandler, ServerDelivery}},
        {"Server delivery -> wire-out", {ServerDelivery, ServerWireOut}},
        {"Server wire-out -> client wire-in", {ServerWireOut, ClientWireIn}},
        {"Client decode", {ClientBeforeDecode, ClientDecode}},
        {"Client decode -> subscriber callback", {ClientDecode, ClientReceive}},
        // The one figure that survives two hosts disagreeing about the time: every phase in it is
        // stamped by the broker itself. Segments that cross a host boundary are only as good as
        // the clock synchronisation, which is milliseconds against a measurement of microseconds.
        {"Server total (reactor ready -> wire-out)", {ServerEventReady, ServerWireOut}},
        {"Total (publish accepted -> subscriber callback)", {ClientSend, ClientReceive}},
    };

    size_t sampleCount;
    size_t rejectedCount;
    size_t unstampedCount;
    {
        const scoped_lock lock(m_mutex);
        sampleCount = m_count;
        rejectedCount = m_rejectedCount;
        unstampedCount = m_unstampedCount;
    }
    const auto reactorNotMeasured = sampleCount > 0 && unstampedCount == sampleCount;

    COUT("Scenario: " << scenarioTitle << " - message path latency breakdown (" << sampleCount << " samples, "
                       << rejectedCount << " rejected as non-monotonic)");
    if (sampleCount == 0)
    {
        COUT("");
        return;
    }

    COUT(std::format("{:<45} {:>10}", "Phase", "Latency, us"));
#ifdef _WIN32
    COUT("-----------------------------------------------------------");
#else
    COUT("─────────────────────────────────────────────────────────────");
#endif
    // Each diff() call takes its own lock; not held across the loop since it also re-locks
    // internally (std::mutex isn't recursive).
    for (const auto& [label, fromTo]: segments)
    {
        if (reactorNotMeasured && (fromTo.first == ServerEventReady || fromTo.second == ServerEventReady))
        {
            COUT(std::format("{:<45} {:>10}", label, "n/a"));
            continue;
        }
        COUT(std::format("{:<45} {:>10}", label, diff(fromTo.first, fromTo.second)));
    }
    if (reactorNotMeasured)
    {
        COUT("n/a: the broker stamps when the reactor woke a session only if it was started with XMQ_LATENCY_TRACE set.");
    }
    else if (unstampedCount != 0)
    {
        COUT(std::format("{} of {} samples carried no reactor stamp and count as no wait for the reactor.", unstampedCount, sampleCount));
    }
    COUT("");
}

void Latency::SNAP_LATENCY(const std::shared_ptr<IMessageProperties>& messageProperties, LatencyPhase phase, LatencyTraceTotal* latencyTraceTotal)
{
    if (messageProperties)
    {
        // Update the trace only if the message originator created it: auto-creating one here
        // would inject a binary user property into messages of clients that never opted in,
        // breaking MQTT 5 clients that enforce UTF-8 validity of user properties.
        if (auto* latencyTrace = messageProperties->getLatencyTrace())
        {
            latencyTrace->snap(phase);
            if (latencyTraceTotal)
            {
                latencyTraceTotal->add(latencyTrace);
            }
        }
    }
}

void Latency::SET_LATENCY(const std::shared_ptr<IMessageProperties>& messageProperties, LatencyPhase phase, const uint64_t valueMcs)
{
    if (messageProperties)
    {
        if (auto* latencyTrace = messageProperties->getLatencyTrace())
        {
            latencyTrace->set(phase, valueMcs);
        }
    }
}

void Latency::COPY_LATENCY(const std::shared_ptr<IMessageProperties>& srcMessageProperties, const std::shared_ptr<IMessageProperties>& dstMessageProperties)
{
    if (srcMessageProperties)
    {
        if (const auto* latencyTrace = srcMessageProperties->getLatencyTrace())
        {
            dstMessageProperties->setLatencyTrace(latencyTrace);
        }
    }
}
