/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2025 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "test/BridgeTests/BridgeTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_F(XMQ_BridgeTests, inBridge)
{
    waitForBridge();

    // Publish to the other server, subscribe to this server:
    auto [subscriber, publisher, topicName] =
        createTestSubscriberAndPublisher(m_otherServerHost, m_xmqServerHost);

    vector<string> received;
    Semaphore      receivedAll;
    subscriber->onMessage(
        [&received, &receivedAll](const SMessage& message)
        {
            received.push_back(message->toString());
            receivedAll.post();
        });

    auto* topic = client::MqttClient::getTopic(topicName);
    publisher->publish(topic, Buffer("Hello, World!"), Qos::Qos1);

    if (!receivedAll.wait_for(5s))
    {
        FAIL() << "Expected message wasn't received";
    }
}

TEST_F(XMQ_BridgeTests, inBridge_performance)
{
    waitForBridge();

    constexpr size_t maxMessageCount = 10000;

    // Publish to the other server, subscribe to this server:
    auto [subscriber, publisher, topicName] =
        createTestSubscriberAndPublisher(m_otherServerHost, m_xmqServerHost);

    Semaphore receivedAll;
    size_t    messageCount = 0;
    subscriber->onMessage(
        [&messageCount, &receivedAll](const SMessage& message)
        {
            if (message->is(Message::Type::Publish))
            {
                ++messageCount;
            }

            if (messageCount == maxMessageCount)
            {
                receivedAll.post();
            }
        });

    auto* topic = client::MqttClient::getTopic(topicName);

    Stopwatch stopwatch;
    stopwatch.start();

    for (size_t i = 0; i < maxMessageCount; ++i)
    {
        publisher->publish(topic, Buffer("Hello, World!"), Qos::Qos1);
    }

    if (!receivedAll.wait_for(10s))
    {
        FAIL() << "Expected message wasn't received";
    }

    stopwatch.stop();
    COUT(maxMessageCount << setprecision(2)
                         << " messages passed for " << stopwatch.seconds() << " sec, "
                         << static_cast<double>(maxMessageCount) / stopwatch.milliseconds() << "K msg/s");
}
