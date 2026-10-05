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

#include "test/SysTopicsTests/SysTopicsTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_F(XMQ_SysTopicsTests, Topics_BrokerTime)
{
    using enum SystemStatistics::SysTopicKind;

    map<string, string> receivedValues;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    const Destinations destinations {
        Destination(client::MqttClient::getTopic(SystemStatistics::sysTopicKindToTopic(BrokerTime)), SubscriptionOptions {Qos::Qos1}),
        Destination(client::MqttClient::getTopic(SystemStatistics::sysTopicKindToTopic(BrokerUptime)), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&receivedValues](const SPublishMessage& publishMessage)
                          {
                              receivedValues[string(publishMessage->destination()->fullName())] = publishMessage->payload();
                              COUT("Received message: " << publishMessage->toString());
                          });
    this_thread::sleep_for(1100ms);
    EXPECT_TRUE(receivedValues.size() >= 2 && receivedValues.size() <= 3);
    EXPECT_TRUE(receivedValues.contains("$SYS/broker/uptime"));
    EXPECT_TRUE(receivedValues.contains("$SYS/broker/time"));

    const auto brokerUptime = receivedValues["$SYS/broker/uptime"];
    EXPECT_TRUE(brokerUptime.length() >= 7);

    const auto brokerTime = receivedValues["$SYS/broker/time"];
    COUT("Broker time: " << brokerTime);
    EXPECT_TRUE(brokerTime.length() >= 20);
    subscriber->disconnect();
}

TEST_F(XMQ_SysTopicsTests, Topics_Publish)
{
    using enum SystemStatistics::SysTopicKind;

    const auto receivedTopic = SystemStatistics::sysTopicKindToTopic(BrokerMessagesPublishReceived);
    const auto sentTopic = SystemStatistics::sysTopicKindToTopic(BrokerMessagesPublishSent);

    mutex               receivedMutex;
    map<string, string> receivedValues;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    const Destinations destinations {
        Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions {Qos::Qos1}),
        Destination(client::MqttClient::getTopic(receivedTopic), SubscriptionOptions {Qos::Qos1}),
        Destination(client::MqttClient::getTopic(sentTopic), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&](const SPublishMessage& message)
                          {
                              const scoped_lock lock(receivedMutex);
                              receivedValues[string(message->destination()->fullName())] = message->payload();
                          });

    publisher->publish(topicName, "test", Qos::Qos1);

    // The counters come once a second; waiting for them, rather than for a fixed time, is what
    // keeps this test from taking six seconds.
    const auto counted = [&](const string& topic)
    {
        const auto it = receivedValues.find(topic);
        return it != receivedValues.end() && string2int(it->second) > 0;
    };
    const auto deadline = chrono::steady_clock::now() + 5s;
    auto       arrived = false;
    while (!arrived && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(20ms);
        const scoped_lock lock(receivedMutex);
        arrived = receivedValues.contains(topicName) && counted(receivedTopic) && counted(sentTopic);
    }

    const scoped_lock lock(receivedMutex);
    EXPECT_EQ("test", receivedValues[topicName]);
    EXPECT_TRUE(counted(receivedTopic)) << receivedTopic << " never reported a publication";
    EXPECT_TRUE(counted(sentTopic)) << sentTopic << " never reported a publication";
}
