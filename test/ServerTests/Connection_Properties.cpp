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

#include "ExternalClient/ExternalClient.h"
#include "common/ConnectAckMessage.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_P(XMQ_ServerTests, Connection_Properties_MQTT5)
{
    if (auto protocolVersion = GetParam();
        protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    auto logger = debugLog(false);

    const map<string, string, std::less<>> properties = {
        {"authentication-data", "data"},
        {"authentication-method", "method"},
        {"maximum-packet-size", "12345"},
        {"receive-maximum", "2345"},
        {"request-problem-information", "1"},
        {"request-response-information", "1"},
        {"session-expiry-interval", "1234"},
        {"topic-alias-maximum", "4567"},
        {"user-property", "name=value"},
    };

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    ExternalClient client(ExternalClient::ClientKind::Mosquitto, subscriberClientId);

    auto subscriber = client.startSubscriber(topicName, Qos::Qos0, 1,
                                             ExternalClient::SessionMode::Clean,
                                             properties,
                                             ExternalClient::OutputMode::Quiet, 1s);
#ifdef _WIN32
    // Allow mosquitto_sub to start and subscribe
    this_thread::sleep_for(2500ms);
#else
    // Allow subscriber utility to start and subscribe
    this_thread::sleep_for(500ms);
#endif

    if (auto clientSession = server()->getClientSession(subscriberClientId))
    {
        if (const auto connectProperties = clientSession->getConnectProperties())
        {
            string_view stringValue;
            EXPECT_TRUE(connectProperties->getProperty(Property::AuthenticationMethod, stringValue));
            // Not .data(): a view into the wire bytes has no terminator behind it.
            EXPECT_EQ("method", stringValue);
        }
    }
    else
    {
        FAIL() << "Client " << subscriberClientId << " is not connected";
    }

    auto client2 = make_shared<client::MqttClient>();

    const ConnectCredentials credentials {publisherClientId, "user", "secret"};
    auto                     rc = client2->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                   {
                                                       .m_lastWillInfo = {},
                                                       .m_cleanSession = true,
                               },
                                                   ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);

    this_thread::sleep_for(TinyTimeout);

    client2->publish(topicName, "text");

    client2->disconnect();

    subscriber->wait();
}

/**
 * @brief Test that the server ignores messages bigger than the maximum packet size.
 */
TEST_F(XMQ_ServerTests, Connection_Properties_MaxPacketSize_MQTT5)
{
    constexpr size_t maxPacketSize = 64;
    auto             protocolVersion = ProtocolVersion::MqttV5;
    int              receivedMessageCount = 0;
    Semaphore        semaphore;

    client::MqttClient subscriber(logEngine());

    subscriber.onMessage(
        [&semaphore, &receivedMessageCount](const SPublishMessage& publishMessage)
        {
            if (string(publishMessage->payload()) == "This is a test message")
            {
                semaphore.post();
            }
            ++receivedMessageCount;
        });

    auto qos = Qos::Qos1;

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    auto                     subscriberProperties = make_shared<MessageProperties>();
    subscriberProperties->setProperty(Property::MaximumPacketSize, maxPacketSize);

    auto rc = subscriber.connect(Host("localhost", TestTcpPortNumber), credentials, {
                                                                                        .m_lastWillInfo = {},
                                                                                        .m_cleanSession = true,
                                                                                    },
                                 protocolVersion, subscriberProperties);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(subscriber.isConnected());

    const Destinations destinations {
        Destination(client::MqttClient::getTopic("topic1")),
        Destination(client::MqttClient::getTopic("topic2")),
    };

    subscriber.subscribe(destinations);
    this_thread::sleep_for(100ms);

    client::MqttClient sender(logEngine());

    const ConnectCredentials credentials2 {publisherClientId, "user", "secret"};
    rc = sender.connect(Host("localhost", TestTcpPortNumber), credentials2,
                        {
                            .m_lastWillInfo = {},
                            .m_cleanSession = true,
                        },
                        protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(sender.isConnected());

    string         largePayload;
    constexpr auto maxPacketSizeExcess = 16;
    largePayload.resize(maxPacketSize + maxPacketSizeExcess);
    for (size_t i = 0; i < maxPacketSize + maxPacketSizeExcess; ++i)
    {
        constexpr auto startingCharacter = 32;
        constexpr auto characterRange = 64;
        largePayload[i] = static_cast<char>(i % characterRange + startingCharacter);
    }

    sender.publish("topic1", largePayload, qos);
    sender.publish("topic1", "This is a test message", qos);

    if (!semaphore.wait_for(chrono::seconds(3)))
    {
        FAIL() << "Message receive timeout";
    }

    EXPECT_EQ(1, receivedMessageCount);

    subscriber.disconnect();
    sender.disconnect();
}

/**
 * @brief Test that the server ignores messages bigger than the maximum packet size.
 */
TEST_P(XMQ_ServerTests, Connection_Properties_InvalidResponseTopicName)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    Semaphore semaphore;

    auto               reasonCode {ReasonCode::Success};
    client::MqttClient client(logEngine());

    client.onAck(
        [&semaphore, &reasonCode](const SMessage& message)
        {
            if (message->is(Message::Type::ConnectAck))
            {
                const auto connectAckMessage = dynamic_pointer_cast<ConnectAckMessage>(message);
                reasonCode = connectAckMessage->getReasonCode();
                semaphore.post();
            }
        });

    const auto senderProperties = make_shared<MessageProperties>();
    senderProperties->setProperty(Property::ResponseTopic, "topic/#");

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    reasonCode = client.connect(Host("localhost", TestTcpPortNumber), credentials,
                                {
                                    .m_lastWillInfo = {},
                                    .m_cleanSession = true,
                                },
                                protocolVersion, senderProperties);
    this_thread::sleep_for(100ms);

    EXPECT_EQ(ReasonCode::ProtocolError, reasonCode);
    EXPECT_FALSE(client.isConnected());
    client.hangup();
}
