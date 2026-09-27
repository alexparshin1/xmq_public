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

#include "client/MqttClient.h"
#include "client/Session.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace xmq::client;

void XMQ_ServerTests::testMqttClientPublish(ProtocolVersion protocolVersion, Qos qos, size_t messageCount)
{
    const Host serverHost("localhost", TestTcpPortNumber);
    auto       logger = debugLog(false);
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    Semaphore     publishSemaphore;
    atomic_size_t messageCountActual {0};

    auto client = make_shared<MqttClient>(logEngine());

    client->onAck([&publishSemaphore, &messageCountActual, messageCount](const SMessage& message)
                  {
                      if (message->is(Message::Type::PublishAck) || message->is(Message::Type::PublishComplete))
                      {
                          ++messageCountActual;
                          if (messageCountActual == messageCount)
                          {
                              publishSemaphore.post();
                          }
                      }
                  });

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(serverHost, credentials, {.m_cleanSession = true}, protocolVersion));
    ASSERT_TRUE(client->isConnected());

    Buffer     payload("This is a test message");
    atomic_int sendMessageCount = 0;
    Stopwatch  receiveStopwatch;

    receiveStopwatch.start();

    for (size_t i = 0; i < messageCount; ++i)
    {
        client->publish(MqttClient::getTopic(topicName), payload, qos);
        ++sendMessageCount;
    }

    EXPECT_TRUE(publishSemaphore.wait_for(3s));
    receiveStopwatch.stop();
    auto receiveDurationMs = receiveStopwatch.milliseconds();

    COUT("MQTT" << static_cast<int>(protocolVersion) << " QOS" << static_cast<int>(qos) << ": " << fixed << setprecision(1)
                << "Sent " << messageCountActual << " msgs for " << receiveDurationMs << " ms, " << fixed << setprecision(1) << messageCountActual / receiveDurationMs << "K msg/s");

    client->disconnect();
    client.reset();
}

void XMQ_ServerTests::testMqttClientReceive(ProtocolVersion protocolVersion, Qos qos, size_t messageCount)
{
    const Host serverHost("localhost", TestTcpPortNumber);
    auto       logger = debugLog(false);
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    Semaphore     subscribedSemaphore;
    Semaphore     publishSemaphore;
    atomic_size_t messageCountActual {0};

    auto subscriber = make_shared<MqttClient>(logEngine());
    auto publisher = make_shared<MqttClient>(logEngine());

    subscriber->onMessage([&messageCountActual, &publishSemaphore, messageCount](const SPublishMessage&)
                          {
                              ++messageCountActual;
                              if (messageCountActual == messageCount)
                              {
                                  publishSemaphore.post();
                              }
                          });

    subscriber->onAck([&subscribedSemaphore](const SMessage& message)
                      {
                          if (message->is(Message::Type::SubscribeAck))
                          {
                              subscribedSemaphore.post();
                          }
                      });

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              subscriber->connect(serverHost, credentials, {.m_cleanSession = true}, protocolVersion));
    ASSERT_TRUE(subscriber->isConnected());

    subscriber->subscribe(Destination(MqttClient::getTopic(topicName), SubscriptionOptions(qos)));
    ASSERT_TRUE(subscribedSemaphore.wait_for(1s));

    const ConnectCredentials credentials2 {publisherClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              publisher->connect(serverHost, credentials2, {.m_cleanSession = true}, protocolVersion));

    Buffer     payload("This is a test message");
    atomic_int sendMessageCount = 0;
    Stopwatch  receiveStopwatch;

    receiveStopwatch.start();

    for (size_t i = 0; i < messageCount; ++i)
    {
        publisher->publish(MqttClient::getTopic(topicName), payload, qos);
        ++sendMessageCount;
    }

    EXPECT_TRUE(publishSemaphore.wait_for(1s));
    receiveStopwatch.stop();
    auto receiveDurationMs = receiveStopwatch.milliseconds();

    COUT("MQTT" << static_cast<int>(protocolVersion) << " QOS" << static_cast<int>(qos) << ": " << fixed << setprecision(1));
    COUT("Received " << messageCountActual << " msgs for " << receiveDurationMs << " ms, " << fixed << setprecision(1) << messageCountActual / receiveDurationMs << "K msg/s");

    this_thread::sleep_for(10ms);

    subscriber->disconnect();
    subscriber.reset();
}

// Test checks if the client can connect, disconnect, and reconnect to the server
TEST_P(XMQ_ServerTests, MqttClient_Publish)
{
    constexpr size_t messageCount = 1000;
    const auto       protocolVersion = GetParam();
    testMqttClientPublish(protocolVersion, Qos::Qos1, messageCount);
    testMqttClientPublish(protocolVersion, Qos::Qos2, messageCount);
}

// Test checks if the client can connect, sendDisconnect, and reconnect to the server
TEST_P(XMQ_ServerTests, MqttClient_PublishReceive)
{
    constexpr size_t messageCount = 1;
    const auto       protocolVersion = GetParam();
    testMqttClientReceive(protocolVersion, Qos::Qos0, messageCount);
    //testMqttClientReceive(protocolVersion, Qos::Qos1, messageCount);
    //testMqttClientReceive(protocolVersion, Qos::Qos2, messageCount);
}
