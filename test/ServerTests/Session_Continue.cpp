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

#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ExternalClient/ExternalClient.h"
#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTests::testSessionReconnect(bool cleanSession)
{
    constexpr auto qos = Qos::Qos1;
    const auto     protocolVersion = GetParam();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // Subscribe a client to the topic and exit
    auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, cleanSession, false, protocolVersion);
    ASSERT_TRUE(subscriber->isConnected());
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(qos)), {});

    // Receive any messages that may have been sent before the subscriber was connected
    this_thread::sleep_for(100ms);
    subscriber->disconnect();

    // Publish one message while the subscriber is not connected
    const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion);
    ASSERT_TRUE(publisher->isConnected());
    publisher->publish(topicName, "Test Data", qos);

    this_thread::sleep_for(50ms);

    // Connect and subscribe again, and try receiving the second message.
    // For a persistent session, don't re-subscribe to the same topic as we expect subscriptions to be preserved.
    subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, cleanSession, false, protocolVersion);
    ASSERT_TRUE(subscriber->isConnected());
    size_t messageCount = 0;
    subscriber->onMessage([&messageCount](const SPublishMessage&)
                          {
                              ++messageCount;
                          });
    if (cleanSession)
    {
        subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(qos)), {});
    }
    this_thread::sleep_for(100ms);

    if (cleanSession)
    {
        EXPECT_EQ(0U, messageCount);
    }
    else
    {
        EXPECT_EQ(1U, messageCount);
    }
}

TEST_P(XMQ_ServerTests, Session_Clean)
{
    testSessionReconnect(true);
}

TEST_P(XMQ_ServerTests, Session_Continue)
{
    testSessionReconnect(false);
}

TEST_F(XMQ_ServerTests, Session_Expiration)
{
    constexpr auto protocolVersion = ProtocolVersion::MqttV5;

    debugLog(false);

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    Semaphore subscribed;
    client->onAck([&subscribed](const SMessage& message)
                  {
                      if (message->is(Message::Type::SubscribeAck))
                      {
                          subscribed.post();
                      }
                  });

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");
    const auto               properties = make_shared<MessageProperties>();
    properties->setProperty(Property::SessionExpiryInterval, 1);
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestTcpPortNumber), credentials,
                              {
                                  .m_keepAliveInterval = 60s,
                                  .m_cleanSession = false,
                              },
                              protocolVersion, properties));

    this_thread::sleep_for(10ms);

    const Destinations destinations({Destination(client::MqttClient::getTopic(topicName))});
    client->subscribe(destinations, properties);

    this_thread::sleep_for(10ms);
    client->disconnect();

    // Check if the session exists:
    auto clientSession = server()->getClientSession(subscriberClientId);
    ASSERT_TRUE(clientSession);

    this_thread::sleep_for(1100ms);

    clientSession = server()->getClientSession(subscriberClientId);
    if (clientSession)
    {
        FAIL() << "Session " << subscriberClientId << " should be expired";
    }
}
