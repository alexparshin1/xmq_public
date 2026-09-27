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

#include "scenario/ScenarioEngine.h"
#include "test/ServerTests/ServerTests.h"

namespace xmq {

enum class ScenarioClass : uint8_t
{
    Basic,
    Enterprise
};

/**
 * @brief Server performance tests.
 */
class XMQ_EXPORT XMQ_ServerPerformanceTests : public XMQ_ServerTests
{
public:
#ifdef BUILD_WITH_COVERAGE
    static constexpr auto TestMessageCount = 10;
    static constexpr auto TestSenderCount = 2;
#elif defined(XMQ_FULL_SCALE_TESTS)
    // The scale these tests were written at: a million messages from a thousand publishers.
    // Build with -DXMQ_FULL_SCALE_TESTS to measure at it.
    static constexpr auto TestMessageCount = 1000;
    static constexpr auto TestSenderCount = 1000;
#else
    // A tenth of the messages from a fifth of the publishers, so the suite stays usable on a
    // development machine. Enough concurrency to catch races and regressions; the published
    // throughput figures come from xmq_scn, not from here.
    static constexpr auto TestMessageCount = 500;
    static constexpr auto TestSenderCount = 200;
#endif

    using ScenarioMap = std::map<ScenarioEngine::Type, std::shared_ptr<ScenarioEngine>>;

    static void testPublishPerformance(const std::string& serverName, uint16_t port, uint32_t messageCount, Qos qos,
                                       ProtocolVersion protocolVersion, ExternalClient::EncryptionMode encryptionMode,
                                       bool useSharedQueue);

    /**
     * @brief Measure aggregate QoS1 receive rate when one logical consumer is spread across
     *        @p subscriberCount connections via a shared subscription ($share/group).
     *
     * Demonstrates that the single-subscriber delivery ceiling (Factor 1) is lifted by adding
     * subscriber connections, since each connection is its own socket / send path.
     * @return Aggregate received messages per second.
     */
    static double testSharedSubscriptionScaling(uint16_t port, uint32_t messageCount, ProtocolVersion protocolVersion,
                                                size_t subscriberCount);
};

} // namespace xmq
