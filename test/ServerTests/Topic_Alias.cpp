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

#include "common/ConnectAckMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTestsLink::linkTopicAliasTests()
{
    // Force linking this module
}

TEST_P(XMQ_ServerTests, TopicAlias_MaxTopicAlias)
{
    if (const auto protocolVersion = GetParam();
        protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    Semaphore          ackReceived;
    client::MqttClient client(logEngine());

    int64_t topicAliasMaximum = 0;
    client.onAck(
        [&topicAliasMaximum, &ackReceived](const SMessage& message)
        {
            if (message->is(Message::Type::ConnectAck))
            {
                const auto connectAckMessage = dynamic_pointer_cast<ConnectAckMessage>(message);
                topicAliasMaximum = 0;
                if (connectAckMessage->getProperties())
                {
                    (void) connectAckMessage->getProperties()->getProperty(Property::TopicAliasMaximum, topicAliasMaximum);
                }
                COUT("TopicAliasMaximum: " << topicAliasMaximum);
                ackReceived.post();
            }
        });

    // What the client is willing to accept, which is not what the CONNACK answers with. MQTT 5,
    // 3.2.2.3.8: the Topic Alias Maximum of a CONNACK is the highest alias the *server* will accept
    // from this client. The two numbers are unrelated, and this test used to require them equal -
    // it asserted the echo that made the broker report the client's own limit back to it.
    const auto     properties = make_shared<MessageProperties>();
    constexpr auto clientTopicAliasMaximum = 32768;
    properties->setProperty(Property::TopicAliasMaximum, clientTopicAliasMaximum);

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client.connect(Host("localhost", TestTcpPortNumber), credentials,
                  {
                  .m_keepAliveInterval = 60s,
                  .m_cleanSession = false,
                  },
                  ProtocolVersion::MqttV5, properties));

    if (!ackReceived.wait_for(MediumTimeout))
    {
        FAIL() << "ConnectAck timeout";
    }

    client.disconnect();

    EXPECT_EQ(server()->getSettings()->m_server_limits.m_max_topic_alias.asInteger(), topicAliasMaximum)
        << "the CONNACK should carry the server's topic alias maximum";
    EXPECT_NE(clientTopicAliasMaximum, topicAliasMaximum) << "the client's own maximum was echoed back";
}

namespace {
auto createEnvironment(Semaphore& messageReceived, const SLogger& logger)
{
    auto sender = make_shared<client::MqttClient>(XMQ_ServerTests::logEngine());
    auto subscriber = make_shared<client::MqttClient>(XMQ_ServerTests::logEngine());

    subscriber->onMessage(
        [&messageReceived, logger](const SPublishMessage& publishMessage)
        {
            logger->info(publishMessage->toString());
            messageReceived.post();
        });

    auto           senderConnectProperties = make_shared<MessageProperties>();
    constexpr auto topicAliasMaximumValue = 16;
    senderConnectProperties->setProperty(Property::TopicAliasMaximum, topicAliasMaximumValue);

    auto [publisherClientId, subscriberClientId, topicName] = XMQ_ServerTests::makeTestNames();

    constexpr auto sixtySeconds = 60s;

    const ConnectCredentials credentials{subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              subscriber->connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber),
                  credentials, {.m_cleanSession = true}, ProtocolVersion::MqttV5));

    const ConnectCredentials credentials2{publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              sender->connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber), credentials2,
                  {
                  .m_keepAliveInterval = sixtySeconds,
                  .m_cleanSession = true,
                  },
                  ProtocolVersion::MqttV5,
                  senderConnectProperties));

    const Destination topic(client::MqttClient::getTopic("topic1"));
    // EXPECT rather than ASSERT: this helper returns the two clients, so it cannot return early.
    EXPECT_TRUE(test::subscribeAndWait(subscriber, topic))
        << "The broker did not acknowledge the subscription";

    auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, 1);

    sender->publish(client::MqttClient::getTopic("topic1"), Buffer("Setting alias"), Qos::Qos0, properties);

    this_thread::sleep_for(10ms);
    if (!messageReceived.wait_for(1s))
    {
        throw Exception("Publish delivery timeout (1)");
    }

    return tuple(sender, subscriber);
}
} // namespace


TEST_P(XMQ_ServerTests, TopicAlias_Define)
{
    if (const auto protocolVersion = GetParam();
        protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    Semaphore messageReceived;

    const auto clientLogger = debugLog(false);

    auto [sender, subscriber] = createEnvironment(messageReceived, clientLogger);

    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, 1);
    sender->publish(client::MqttClient::getTopic(""), Buffer("Hello Alias"), Qos::Qos0, properties);

    if (!messageReceived.wait_for(1000s))
    {
        FAIL() << "Publish delivery timeout (2)";
    }

    sender->disconnect();
    subscriber->disconnect();
}

TEST_F(XMQ_ServerTests, TopicAlias_AnotherConnection)
{
    Semaphore messageReceived;

    const auto clientLogger = debugLog(false);

    auto [sender, subscriber] = createEnvironment(messageReceived, clientLogger);

    auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, 1);
    sender->publish(client::MqttClient::getTopic(""), Buffer("Hello Alias"), Qos::Qos0, properties);

    if (!messageReceived.wait_for(1s))
    {
        FAIL() << "Publish delivery timeout (2)";
    }

    sender->disconnect();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              sender->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = true}));

    properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, 1);
    sender->publish(client::MqttClient::getTopic(""), Buffer("Hello Alias 2"), Qos::Qos0, properties);

    if (messageReceived.wait_for(TinyTimeout))
    {
        FAIL() << "Topic alias carried to another connection";
    }

    sender->disconnect();
    subscriber->disconnect();
}

TEST_F(XMQ_ServerTests, TopicAlias_InvalidAlias)
{
    Semaphore messageReceived;

    const auto clientLogger = make_shared<Logger>(*logEngine());

    auto [sender, subscriber] = createEnvironment(messageReceived, clientLogger);

    auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, 0);
    sender->publish(client::MqttClient::getTopic(""), Buffer("Hello Alias"), Qos::Qos0, properties);

    if (messageReceived.wait_for(TinyTimeout))
    {
        FAIL() << "Message should not be delivered";
    }
    EXPECT_FALSE(sender->isConnected());
    subscriber->disconnect();

    auto [sender2, subscriber2] = createEnvironment(messageReceived, clientLogger);

    constexpr auto invalidAlias = 17;
    properties = make_shared<MessageProperties>();
    properties->setProperty(Property::TopicAlias, invalidAlias);
    sender2->publish(client::MqttClient::getTopic("topic1"), Buffer("Hello Alias"), Qos::Qos0, properties);

    if (messageReceived.wait_for(TinyTimeout))
    {
        FAIL() << "Message should not be delivered";
    }
    EXPECT_FALSE(sender2->isConnected());
    subscriber2->disconnect();
}