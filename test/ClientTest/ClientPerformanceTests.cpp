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

#include "MqttClientTests.h"
#include "client/MqttClient.h"
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

size_t clientThread(const shared_ptr<LogEngine>& logEngine, const ConnectCredentials& credentials,
                    const Destinations& destinations, const Buffer& data, size_t expectedMessageCounter)
{
    size_t messageCounter = 0;

    client::MqttClient client(logEngine);

    Semaphore publishSemaphore;

    client.onMessage(
        [&publishSemaphore, &messageCounter, expectedMessageCounter](const SPublishMessage&)
        {
            ++messageCounter;
            if (messageCounter == expectedMessageCounter)
            {
                publishSemaphore.post();
            }
        });

    const auto rc = client.connect(*XMQ_MqttClientTests::m_mqttHost, credentials, {.m_cleanSession = true});
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());

    client.subscribe(destinations, {});
    this_thread::sleep_for(10ms);

    Stopwatch stopwatch;
    stopwatch.start();

    for (size_t i = 0; i < expectedMessageCounter; ++i)
    {
        client.publish(client::MqttClient::getTopic("topic1"), data, Qos::Qos1);
    }

    publishSemaphore.wait_for(5s);

    client.unsubscribe(destinations);

    client.disconnect();

    return messageCounter;
}
} // namespace

TEST_F(XMQ_MqttClientTests, performance)
{
    const auto               logEngine = createLogEngine(LogPriority::Info);
    const ConnectCredentials credentials {"test_client", "user", "secret"};

    const Destinations destinations {
        Destination(client::MqttClient::getTopic("topic1"), SubscriptionOptions(Qos::Qos1)),
    };

    constexpr size_t expectedMessageCounter = 100;
    constexpr size_t messageSize = 40;

    Buffer data;
    while (data.size() < messageSize)
    {
        data.append("This is a test message. ");
    }
    data.bytes(messageSize);

    Stopwatch stopwatch;
    stopwatch.start();

    const auto messageCounter = clientThread(logEngine, credentials, destinations, data, expectedMessageCounter);

    EXPECT_EQ(expectedMessageCounter, messageCounter);

    stopwatch.stop();
    COUT("Time elapsed: " << fixed << setprecision(1) << stopwatch.milliseconds() << "ms, " << expectedMessageCounter / stopwatch.milliseconds() << "K messages per second");
}
