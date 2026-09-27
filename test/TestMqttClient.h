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

#include "client/MqttClient.h"
#include "test/ServerTests_Suite.h"
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief A test MQTT client.
 */
class TestMqttClient final : public client::MqttClient
{
public:
    /**
     * @brief Constructor
     * @param logEngine         Log engine
     * @param clientId          Client ID
     * @param cleanSession      Clean session flag
     * @param anonymous         Anonymous mode
     * @param protocolVersion   Protocol version
     * @param messageProperties Connect message properties
     * @param portNumber        Port number, ignored when useTls is set
     * @param useTls            Connect over TLS, to the encrypted listener rather than portNumber
     */
    TestMqttClient(const std::shared_ptr<sptk::LogEngine>& logEngine, std::string_view clientId, bool cleanSession = true,
                   bool anonymous = false, ProtocolVersion protocolVersion = ProtocolVersion::MqttV5,
                   const SMessageProperties& messageProperties = {}, uint16_t portNumber = ServerTests_Suite::TestTcpPortNumber,
                   bool useTls = false);

    /**
     * @brief Destructor
     */
    ~TestMqttClient() override = default;

    /**
     * @brief Publish multiple messages
     * @param topic             Destination topic
     * @param payload           Message payload
     * @param count             Number of messages
     * @param qos               QoS
     */
    void publishMultiple(std::string_view topic, const sptk::Buffer& payload, size_t count, Qos qos);
};

using STestMqttClient = std::shared_ptr<TestMqttClient>;

} // namespace xmq
