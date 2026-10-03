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

/**
 * @brief $SYS topics restrictions for 'subscribe' and 'publish' messages.
 *
 * The restrictions are described in https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html,
 * section 4.7.2.
 */

/**
 * @brief Client messages published to $SYS topics should be ignored.
 */
TEST_F(XMQ_SysTopicsTests, Topics_PublishIgnored)
{
    using enum SystemStatistics::SysTopicKind;

    int maxTotalClients = 0;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    const Destinations destinations {
        Destination(client::MqttClient::getTopic(SystemStatistics::sysTopicKindToTopic(BrokerClientsTotal)), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&maxTotalClients](const SPublishMessage& publishMessage)
                          {
                              if (const auto topic = publishMessage->destination()->fullName();
                                  topic == "$SYS/broker/clients/total")
                              {
                                  const auto payload = publishMessage->payload();
                                  if (const auto totalClients = String(payload.data(), payload.size()).toInt();
                                      maxTotalClients < totalClients)
                                  {
                                      maxTotalClients = totalClients;
                                  }
                              }
                              COUT("Received message: " << publishMessage->toString());
                          });

    publisher->publish(SystemStatistics::sysTopicKindToTopic(BrokerClientsTotal),
                       "1000000", Qos::Qos1);

    // Wait till system topics are updated
    this_thread::sleep_for(1100ms);
    subscriber->onMessage({});
    EXPECT_GE(100, maxTotalClients);

    publisher->disconnect();
    subscriber->disconnect();
}

/**
 * @brief A subscription to “#” will not receive any messages from a topic beginning with a $SYS.
 */
TEST_F(XMQ_SysTopicsTests, Topics_SubscriptionToAll)
{
    using enum SystemStatistics::SysTopicKind;

    int sysMessageReceived = 0;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();

    // Subscribe to all topics.
    const Destinations destinations {
        Destination(client::MqttClient::getTopic("#"), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&sysMessageReceived](const SPublishMessage& publishMessage)
                          {
                              if (const auto topic = publishMessage->destination()->fullName();
                                  topic.starts_with("$SYS/broker/"))
                              {
                                  ++sysMessageReceived;
                              }

                              COUT("Received message: " << publishMessage->toString());
                          });

    publisher->publish("topic/1", "Test message", Qos::Qos1);

    // Wait till system topics are updated
    this_thread::sleep_for(1100ms);
    EXPECT_EQ(0, sysMessageReceived);

    publisher->disconnect();
    subscriber->disconnect();
}

/**
 * @brief A subscription starting from “+” will not receive any messages from a topic beginning with a $SYS.
 */
TEST_F(XMQ_SysTopicsTests, Topics_SubscriptionToPlus)
{
    using enum SystemStatistics::SysTopicKind;

    int sysMessageReceived = 0;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();

    // Subscribe to all topics.
    const Destinations destinations {
        Destination(client::MqttClient::getTopic("+/broker/clients/total"), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&sysMessageReceived](const SPublishMessage& message)
                          {
                              if (const auto topic = message->destination()->fullName();
                                  topic.starts_with("$SYS/broker/"))
                              {
                                  ++sysMessageReceived;
                              }

                              COUT("Received message: " << message->toString());
                          });

    publisher->publish("topic/1", "Test message", Qos::Qos1);

    // Wait till system topics are updated
    this_thread::sleep_for(100ms);
    EXPECT_EQ(0, sysMessageReceived);

    publisher->disconnect();
    subscriber->disconnect();
}

/**
 * @brief A subscription starting from “$SYS” will respect "#".
 */
TEST_F(XMQ_SysTopicsTests, Topics_SysSubscriptionToAll)
{
    using enum SystemStatistics::SysTopicKind;

    int sysMessageReceived = 0;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();

    // Subscribe to all topics.
    const Destinations destinations {
        Destination(client::MqttClient::getTopic("$SYS/#"), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&sysMessageReceived](const SPublishMessage& message)
                          {
                              if (const auto topic = message->destination()->fullName();
                                  topic.starts_with("$SYS/broker/"))
                              {
                                  ++sysMessageReceived;
                              }

                              COUT("Received message: " << message->toString());
                          });

    publisher->publish("topic/1", "Test message", Qos::Qos1);

    // Wait till system topics are updated
    this_thread::sleep_for(1100ms);
    EXPECT_NE(0, sysMessageReceived);

    publisher->disconnect();
    subscriber->disconnect();
}

/**
 * @brief A subscription starting from “$SYS” will respect "+".
 */
TEST_F(XMQ_SysTopicsTests, Topics_SysSubscriptionToWildcard)
{
    using enum SystemStatistics::SysTopicKind;

    int sysMessageReceived = 0;

    const auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();

    // Subscribe to all topics.
    const Destinations destinations {
        Destination(client::MqttClient::getTopic("$SYS/broker/clients/+"), SubscriptionOptions {Qos::Qos1})};
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destinations))
        << "The broker did not acknowledge the subscription";
    subscriber->onMessage([&sysMessageReceived](const SPublishMessage& message)
                          {
                              if (const auto topic = message->destination()->fullName();
                                  topic.starts_with("$SYS/broker/clients/"))
                              {
                                  ++sysMessageReceived;
                              }

                              COUT("Received message: " << message->toString());
                          });

    publisher->publish("topic/1", "Test message", Qos::Qos1);

    // Wait till system topics are updated
    this_thread::sleep_for(1200ms);
    EXPECT_NE(0, sysMessageReceived);

    publisher->disconnect();
    subscriber->disconnect();
}
