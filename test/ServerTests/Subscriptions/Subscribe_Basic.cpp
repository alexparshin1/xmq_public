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

#include "base/MessageProperties.h"
#include "common/DisconnectMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr auto testSubscriptionId1 = 12345;
constexpr auto testSubscriptionId2 = 54321;

enum class InvalidPart : uint8_t
{
    QoS,
    SubscriptionId,
    ReservedOptionBits
};

void subscribeErrorTests(const shared_ptr<client::MqttClient>& client, const XMQ_ServerTests::TestNames& testNames, const ProtocolVersion protocolVersion, const InvalidPart invalidPart)
{
    Semaphore clientDisconnected;

    auto reasonCode {ReasonCode::Success};

    reasonCode = ReasonCode::Success;

    const auto    qos = invalidPart == InvalidPart::QoS ? Qos::Invalid : Qos::Qos0;
    uint8_t const reservedBits = invalidPart == InvalidPart::ReservedOptionBits ? 3 : 0;

    client->onDisconnect(
        [&clientDisconnected, &reasonCode](const SMessage& message)
        {
            const auto disconnect = dynamic_pointer_cast<DisconnectMessage>(message);
            reasonCode = disconnect->getReasonCode();
            clientDisconnected.post();
        });

    const ConnectCredentials credentials {testNames.m_subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber), credentials, {.m_cleanSession = true}, protocolVersion));
    ASSERT_TRUE(client->isConnected());

    this_thread::sleep_for(XMQ_ServerTests::TinyTimeout);

    // Destination with invalidPart QoS:
    Destination destination(client::MqttClient::getTopic(testNames.m_topicName), SubscriptionOptions(qos));

    const auto properties = make_shared<MessageProperties>();
    if (invalidPart == InvalidPart::SubscriptionId)
    {
        properties->addSubscriptionId(0);
    }
    destination.m_subscribeOptions.m_reserved = reservedBits;

    // Sent without waiting, deliberately: this SUBSCRIBE is malformed on purpose and the broker is
    // expected to answer it by dropping the connection, so there is no SUBACK to wait for.
    client->subscribe(destination, properties);

    if (!clientDisconnected.wait_for(1s))
    {
        client->hangup();
        FAIL() << "Disconnect is not received in time";
    }

    client->disconnect();

    if (protocolVersion == ProtocolVersion::MqttV5)
    {
        EXPECT_TRUE(reasonCode == ReasonCode::MalformedPacket || reasonCode == ReasonCode::ProtocolError);
    }
    else
    {
        EXPECT_EQ(ReasonCode::Success, reasonCode);
    }
}

} // namespace

TEST_P(XMQ_ServerTests, Subscribe_InvalidQoS)
{
    const auto protocolVersion = GetParam();
    const auto client = make_shared<client::MqttClient>();
    const auto testNames = makeTestNames();

    subscribeErrorTests(client, testNames, protocolVersion, InvalidPart::QoS);
}

TEST_P(XMQ_ServerTests, Subscribe_InvalidReservedBits)
{
    const auto protocolVersion = GetParam();

    // The reserved bits behavior is only defined in MQTT 5.0
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    const auto client = make_shared<client::MqttClient>();
    const auto testNames = makeTestNames();

    subscribeErrorTests(client, testNames, protocolVersion, InvalidPart::ReservedOptionBits);
}

TEST_P(XMQ_ServerTests, Subscribe_InvalidSubscriptionId)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    const auto client = make_shared<client::MqttClient>();
    const auto testNames = makeTestNames();

    subscribeErrorTests(client, testNames, protocolVersion, InvalidPart::SubscriptionId);
}

TEST_P(XMQ_ServerTests, Subscribe_WithSubscriptionId)
{
    const auto protocolVersion = GetParam();
    const auto client = make_shared<client::MqttClient>();

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    MessageProperties receivedProperties;
    auto              messageIsReceived = make_shared<Semaphore>();

    client->onMessage([&messageIsReceived, &receivedProperties](const SPublishMessage& message)
                      {
                          if (message->getProperties())
                          {
                              receivedProperties = *dynamic_cast<MessageProperties*>(message->getProperties().get());
                          }
                          messageIsReceived->post();
                      });

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = true}, protocolVersion));

    this_thread::sleep_for(TinyTimeout);

    // Destination 1:
    Destinations const destinations({Destination(client::MqttClient::getTopic(topicName + "/1"))});
    auto               properties = make_shared<MessageProperties>();
    properties->setProperty(Property::SubscriptionIdentifier, testSubscriptionId1);
    ASSERT_TRUE(test::subscribeAndWait(client, destinations, properties))
        << "The broker did not acknowledge the subscription";

    // Destination 2:
    Destinations const destinations2({Destination(client::MqttClient::getTopic(topicName + "/#"))});
    auto               properties2 = make_shared<MessageProperties>();
    properties2->setProperty(Property::SubscriptionIdentifier, testSubscriptionId2);
    ASSERT_TRUE(test::subscribeAndWait(client, destinations2, properties2))
        << "The broker did not acknowledge the subscription";

    this_thread::sleep_for(TinyTimeout);

    client->publish(topicName + "/1", "Test Data");

    if (!messageIsReceived->wait_for(1s))
    {
        FAIL() << "Message not received";
    }

    if (protocolVersion == ProtocolVersion::MqttV5)
    {
        // For MQTT5, the subscription identifiers are included in the properties
        const set<uint32_t> expectedSubscriptionIds = {testSubscriptionId1, testSubscriptionId2};
        EXPECT_EQ(expectedSubscriptionIds, receivedProperties.getSubscriptionIds());
    }
    else
    {
        // For MQTT3, the properties should be null
        const set<uint16_t> expectedSubscriptionIds;
        EXPECT_EQ(0u, receivedProperties.expectedSize());
    }

    client->disconnect();
}
