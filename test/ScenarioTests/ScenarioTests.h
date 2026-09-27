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

/**
 * @brief Server performance tests.
 */
class XMQ_EXPORT XMQ_ScenarioTests : public XMQ_ServerTests
{
public:
    using ScenarioMap = std::map<ScenarioEngine::Type, std::shared_ptr<ScenarioEngine>>;

    XMQ_ScenarioTests()
    {
        loadScenarios();
    }

    /**
     * @brief Load all scenarios.
     */
    static void loadScenarios();

    static std::shared_ptr<ScenarioEngine> getScenario(ScenarioEngine::Type scenario);

    /**
     * @brief The same scenario, pointed at the encrypted listener and connecting over TLS.
     *
     * A separate set of engines, loaded from the same files: the plain ones are shared between
     * tests and must not acquire keys of their own.
     */
    static std::shared_ptr<ScenarioEngine> getEncryptedScenario(ScenarioEngine::Type scenario);

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

private:
    static ScenarioMap m_basicTestScenarios;
    static ScenarioMap m_encryptedTestScenarios;

    /**
     * @brief Load scenarios from a directory.
     * @param scenarioDirectory Scenario directory.
     * @param scenarios Scenarios map.
     */
    static void loadScenarioGroup(const std::filesystem::path& scenarioDirectory, ScenarioMap& scenarios);
};


} // namespace xmq
