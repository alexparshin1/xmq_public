/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "base/AckMessage.h"
#include "common/DisconnectMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTestsLink::linkPublishPropertiesTests()
{
    // Force linking this module
}

namespace {
constexpr auto testSubscriptionId1 = 12345;
constexpr auto testSubscriptionId2 = 54321;
} // namespace

void XMQ_ServerTests::testInvalidPublishMessageResponse(const ProtocolVersion protocolVersion, const shared_ptr<PublishMessage>& publishMessage, const ReasonCode expectedErrorMqtt3, const ReasonCode expectedErrorMqtt5)
{
    const auto client = make_shared<client::MqttClient>();

    Semaphore isDisconnected;
    auto      disconnectReasonCode {ReasonCode::Success};
    client->onDisconnect(
        [&isDisconnected, &disconnectReasonCode](const SMessage& message)
        {
            const auto disconnect = dynamic_pointer_cast<DisconnectMessage>(message);
            disconnectReasonCode = disconnect->getReasonCode();
            isDisconnected.post();
        });

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials {publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = true}, protocolVersion));

    EXPECT_TRUE(client->isConnected());
    this_thread::sleep_for(TinyTimeout);

    client->publish(*publishMessage);

    if (isDisconnected.wait_for(1s))
    {
        const auto expectedError = protocolVersion == ProtocolVersion::MqttV5 ? expectedErrorMqtt5 : expectedErrorMqtt3;
        EXPECT_EQ(expectedError, disconnectReasonCode);
    }
    else
    {
        FAIL() << "Disconnect timeout";
    }

    client->hangup();
}

TEST_P(XMQ_ServerTests, Publish_InvalidQOS)
{
    const auto protocolVersion = GetParam();

    const auto publishMessage = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("topic1"), string_view("This is a test"));
    publishMessage->setQos(Qos::Invalid);

    testInvalidPublishMessageResponse(protocolVersion, publishMessage, ReasonCode::Success, ReasonCode::MalformedPacket);
}

TEST_P(XMQ_ServerTests, Publish_InvalidDupQOS0)
{
    const auto protocolVersion = GetParam();

    const auto publishMessage = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("topic1"), string_view("This is a test"));
    publishMessage->setQos(Qos::Qos0);
    publishMessage->setDup(true);

    testInvalidPublishMessageResponse(protocolVersion, publishMessage, ReasonCode::Success, ReasonCode::MalformedPacket);
}

void XMQ_ServerTests::postMessageOffline(const ProtocolVersion protocolVersion, const std::shared_ptr<PublishMessage>& publishMessage, const TestNames& testNames)
{
    const auto client = make_shared<client::MqttClient>();
    Semaphore  subscriptionComplete;

    const ConnectCredentials credentials {testNames.m_subscriberClientId, "user", "secret"};

    client->onAck(
        [&subscriptionComplete](const SMessage& message)
        {
            if (message->is(Message::Type::SubscribeAck))
            {
                if (const auto subscribeAck = dynamic_pointer_cast<AckMessage>(message);
                    subscribeAck->getReasonCode() == ReasonCode::Success)
                {
                    subscriptionComplete.post();
                }
            }
        });

    const auto reasonCode = client->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = false}, protocolVersion);

    EXPECT_TRUE(reasonCode == ReasonCode::Success);

    const auto destination = make_shared<Destination>(client::MqttClient::getTopic(testNames.m_topicName));
    client->subscribe(*destination);

    EXPECT_TRUE(subscriptionComplete.wait_for(1000s));
    client->disconnect();

    // Send a message with 10 sec expiration
    client::MqttClient publisher(server()->getLogEngine());

    const ConnectCredentials credentials2 {testNames.m_publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              publisher.connect(Host("localhost", TestTcpPortNumber), credentials2, {.m_cleanSession = true}, protocolVersion));
    EXPECT_TRUE(publisher.isConnected());

    publisher.publish(*publishMessage);
    this_thread::sleep_for(10ms);

    publisher.disconnect();
}

void XMQ_ServerTests::getMessagePostedOffline(const ProtocolVersion protocolVersion, mqtt::SPublishMessage& receivedMessage, const TestNames& testNames)
{
    client::MqttClient client(server()->getLogEngine());

    Semaphore received;
    client.onMessage(
        [&received, &receivedMessage](const SPublishMessage& publishMessage)
        {
            receivedMessage = make_shared<mqtt::PublishMessage>(publishMessage->destination(),
                                                                publishMessage->payload(),
                                                                publishMessage->getId(), publishMessage->isRetain());
            receivedMessage->setProperties(publishMessage->getProperties());
            received.post();
        });

    const ConnectCredentials credentials {testNames.m_subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client.connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = false}, protocolVersion));

    EXPECT_TRUE(client.isConnected());

    const Destination destination(client::MqttClient::getTopic(testNames.m_topicName));
    client.subscribe(destination);

    if (!received.wait_for(100ms))
    {
        FAIL() << "Message timeout";
    }

    client.hangup();
}

/**
 * @brief Check that message expiration (seconds) is passed correctly.
 * The time of message processing and holding on the server should be subtracted from the message expiration.
 */
TEST_P(XMQ_ServerTests, Publish_TestMessageRemainingExpiration)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion < ProtocolVersion::MqttV5)
    {
        // Message expiration is not supported prior to MqttV5
        return;
    }

    const auto testNames = makeTestNames();

    // Create a QoS1 message with 10 sec expiration
    auto publishMessage = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic(testNames.m_topicName), string_view("This is a test"));
    publishMessage->setQos(Qos::Qos1);
    const auto     properties = make_shared<MessageProperties>();
    constexpr auto tenSeconds = 10;
    properties->setProperty(Property::MessageExpiryInterval, tenSeconds);
    publishMessage->setProperties(properties);

    // Create the subscription and disconnect. Then send the message to that subscription and disconnect.
    postMessageOffline(protocolVersion, publishMessage, testNames);

    // Wait for 200 ms to make message expiration change noticeable
    this_thread::sleep_for(SmallTimeout);

    // Subscribe to the same topic and receive the message
    getMessagePostedOffline(protocolVersion, publishMessage, testNames);

    // Verify the expiration is less between 7 and 9 sec
    int64_t remainingExpiration = 0;
    EXPECT_TRUE(publishMessage->getProperties()->getProperty(Property::MessageExpiryInterval, remainingExpiration));
    EXPECT_GE(10U, remainingExpiration);
    EXPECT_LE(7U, remainingExpiration);
}

TEST_P(XMQ_ServerTests, Publish_InvalidResponseTopic)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    const auto publishMessage = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("topic1"), string_view("This is a test"));
    publishMessage->setQos(Qos::Qos0);

    const auto messageProperties = make_shared<MessageProperties>();
    messageProperties->setProperty(Property::ResponseTopic, "topic/#");
    publishMessage->setProperties(messageProperties);

    testInvalidPublishMessageResponse(protocolVersion, publishMessage, ReasonCode::Success, ReasonCode::ProtocolError);
}

void XMQ_ServerTests::passProperty(Property property, string_view value, const SUserMessageProperties& userProperties,
                                   const set<uint32_t>& subscriptionIds)
{
    const auto client = make_shared<client::MqttClient>();

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestTcpPortNumber), credentials,
                              {.m_cleanSession = true}, ProtocolVersion::MqttV5));

    EXPECT_TRUE(client->isConnected());

    Semaphore         messageDelivered;
    MessageProperties deliveredMessageProperties;

    client->onMessage(
        [&messageDelivered, &deliveredMessageProperties](const SPublishMessage& message)
        {
            const auto& properties = message->getProperties();
            const auto* propertiesMqtt5 = dynamic_cast<const MessageProperties*>(properties.get());
            deliveredMessageProperties = *propertiesMqtt5;
            messageDelivered.post();
        });

    const Destination destination(client::MqttClient::getTopic(topicName));
    client->subscribe(destination);
    this_thread::sleep_for(100ms);

    auto publishMessage = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic(topicName), string_view("This is a property test"));
    publishMessage->setQos(Qos::Qos0);

    auto messageProperties = make_shared<MessageProperties>();
    messageProperties->setProperty(property, value);

    if (userProperties)
    {
        messageProperties->setUserProperties(*userProperties);
    }

    messageProperties->setSubscriptionIds(subscriptionIds);

    publishMessage->setProperties(messageProperties);

    client::MqttClient publisher(server()->getLogEngine());

    const ConnectCredentials credentials2 {publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              publisher.connect(Host("localhost", TestTcpPortNumber), credentials2, {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    publisher.publish(*publishMessage);

    EXPECT_TRUE(messageDelivered.wait_for(100ms));
    string_view responseTopic;
    EXPECT_TRUE(deliveredMessageProperties.getProperty(property, responseTopic));
    EXPECT_EQ(value, responseTopic);

    if (userProperties)
    {
        // The maps themselves, not their text: toString() walks an unordered_map, so two
        // equal sets of properties compare unequal as soon as they were inserted in a
        // different order.
        EXPECT_EQ(deliveredMessageProperties.getUserProperties(), *userProperties);
    }

    EXPECT_EQ(deliveredMessageProperties.getSubscriptionIds(), subscriptionIds);

    publisher.hangup();
    client->hangup();
}

TEST_F(XMQ_ServerTests, Publish_PassResponseTopic)
{
    set<uint32_t> const subscriptionIds {testSubscriptionId1, testSubscriptionId2};
    passProperty(Property::ResponseTopic, "topic/my_topic", nullptr, subscriptionIds);
}

TEST_F(XMQ_ServerTests, Publish_PassCorrelationData)
{
    set<uint32_t> const subscriptionIds {testSubscriptionId1, testSubscriptionId2};
    passProperty(Property::CorrelationData, "MSG-123456-TEST", nullptr, subscriptionIds);
}

TEST_F(XMQ_ServerTests, Publish_PassContentType)
{
    set<uint32_t> const subscriptionIds {testSubscriptionId1, testSubscriptionId2};
    passProperty(Property::ContentType, "image/png", nullptr, subscriptionIds);
}

TEST_F(XMQ_ServerTests, Publish_PassUserProperties)
{
    const auto userProperties = make_shared<UserMessageProperties>();
    (*userProperties)["key1"] = "value1";
    (*userProperties)["key2"] = "value2";

    set<uint32_t> const subscriptionIds {testSubscriptionId1, testSubscriptionId2};
    passProperty(Property::CorrelationData, "MSG-123456-TEST", userProperties, subscriptionIds);
}
