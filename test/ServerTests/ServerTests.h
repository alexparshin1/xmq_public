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

#include "ServerTestsLink.h"
#include "ServerTests_Suite.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ExternalClient/ExternalClient.h"

namespace xmq {

enum class RetainTestMode
{
    RegularTopic,
    TopLevelTopic,
    RegularWildcard,
    TopLevelWildcard
};

class XMQ_EXPORT XMQ_ServerTests
    : public ServerTests_Suite
    , public testing::WithParamInterface<ProtocolVersion>
    , public XMQ_ServerTestsLink
{
public:
    static constexpr std::chrono::milliseconds TinyTimeout {20};
    static constexpr std::chrono::milliseconds SmallTimeout {200};
    static constexpr std::chrono::milliseconds MediumTimeout {1000};

    struct TestNames
    {
        std::string m_publisherClientId;
        std::string m_subscriberClientId;
        std::string m_topicName;
    };

    enum class TestSubscribeOption : uint8_t
    {
        TestNoLocal,
        TestRetainAsPublished,
        TestRetainHandling
    };

    static void testPublishPerformance(const sptk::String& serverName, uint16_t port, uint32_t messageCount, Qos qos, ProtocolVersion protocolVersion, ExternalClient::EncryptionMode encryptionMode, bool useSharedQueue);
    static void testSessionReconnect(bool cleanSession);

    /**
     * @brief Test last will and testament delivery.
     * @param protocolVersion       MQTT protocol version.
     * @param gracefulDisconnect    Do graceful disconnect.
     */
    static void testLastWillAndTestament(ProtocolVersion protocolVersion, bool gracefulDisconnect);

    /**
     * @brief Test server response to the invalid Publish message.
     * @param protocolVersion   MQTT protocol version.
     * @param publishMessage    Publish message.
     * @param expectedErrorMqtt3 Expected error code for MQTT 3.
     * @param expectedErrorMqtt5 Expected error code for MQTT 5
     */
    static void testInvalidPublishMessageResponse(ProtocolVersion protocolVersion, const std::shared_ptr<PublishMessage>& publishMessage,
                                                  ReasonCode expectedErrorMqtt3, ReasonCode expectedErrorMqtt5);

    /**
     * @brief Utility method to post the message while the subscriber is offline.
     * Connect to the server and subscribe to the topic, then disconnect.
     * Connect again and publish the message.
     * @param protocolVersion   MQTT protocol version.
     * @param publishMessage    Message to post.
     * @param testNames         Test names.
     */
    static void postMessageOffline(ProtocolVersion protocolVersion, const std::shared_ptr<PublishMessage>& publishMessage, const TestNames& testNames);

    /**
     * @brief Utility method to receive the message posted while the subscriber is offline.
     * @param protocolVersion   MQTT protocol version.
     * @param receivedMessage   Received message.
     * @param testNames         Test names.
     */
    static void getMessagePostedOffline(ProtocolVersion protocolVersion, mqtt::SPublishMessage& receivedMessage, const TestNames& testNames);

    static void passProperty(Property property, std::string_view value, const SUserMessageProperties& userProperties, const std::set<uint32_t>&
                             subscriptionIds);

    [[nodiscard]] static TestNames makeTestNames();

    static void testSubscribeOptions(ProtocolVersion protocolVersion, TestSubscribeOption testOption);
    static void testSubscribeRetainHandling(ProtocolVersion protocolVersion, SubscribeRetainHandling retainHandling);

    static void testMqttClientPublish(ProtocolVersion protocolVersion, Qos qos, size_t messageCount);
    static void testMqttClientReceive(ProtocolVersion protocolVersion, Qos qos, size_t messageCount);

    static void printTiming(std::string_view operationName, size_t messageCount, double seconds);
    static void testRetainMessages(ProtocolVersion protocolVersion, RetainTestMode testMode);
};

} // namespace xmq
