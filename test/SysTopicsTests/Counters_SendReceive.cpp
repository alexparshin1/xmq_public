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
#include "test/SysTopicsTests/SysTopicsTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_F(XMQ_SysTopicsTests, SendReceive_Bytes)
{
    using enum SystemStatistics::SysTopicKind;

    // Counted as a difference from where the broker already stood. These tests used to call
    // stats->clear() first so they could compare against bare numbers, but the statistics belong to
    // the broker every test in this suite shares: clearing them while sessions from earlier tests
    // were still connected sent the connected-clients counter below zero on the next disconnect,
    // where it wrapped to 18446744073709551615 and every later assertion about it was nonsense.
    const auto* stats = server()->systemStatistics();
    const auto  initialBytesReceived = stats->getValue(BrokerLoadBytesReceived);
    const auto  initialBytesSent = stats->getValue(BrokerLoadBytesSent);

    const auto subscriber = createTestSubscriber();
    this_thread::sleep_for(10ms);

    const auto bytesReceived = stats->getValue(BrokerLoadBytesReceived) - initialBytesReceived;
    EXPECT_LE(42U, bytesReceived);
    EXPECT_GE(50U, bytesReceived);

    const auto bytesSent = stats->getValue(BrokerLoadBytesSent) - initialBytesSent;
    EXPECT_EQ(4U, bytesSent);

    subscriber->disconnect();
}

TEST_F(XMQ_SysTopicsTests, SendReceive_PingBytes)
{
    using enum SystemStatistics::SysTopicKind;

    const auto subscriber = createTestSubscriber();
    this_thread::sleep_for(10ms);

    const auto* stats = server()->systemStatistics();

    const auto initialBytesReceived = stats->getValue(BrokerLoadBytesReceived);
    const auto initialBytesSent = stats->getValue(BrokerLoadBytesSent);

    subscriber->ping();
    this_thread::sleep_for(10ms);

    const auto bytesReceived = stats->getValue(BrokerLoadBytesReceived);
    EXPECT_EQ(initialBytesReceived + 2, bytesReceived);

    const auto bytesSent = stats->getValue(BrokerLoadBytesSent);
    EXPECT_EQ(initialBytesSent + 2, bytesSent);

    subscriber->disconnect();
    this_thread::sleep_for(100ms);
}

TEST_F(XMQ_SysTopicsTests, SendReceive_Messages_Connect)
{
    using enum SystemStatistics::SysTopicKind;

    // Counted as a difference from where the broker already stood. These tests used to call
    // stats->clear() first so they could compare against bare numbers, but the statistics belong to
    // the broker every test in this suite shares: clearing them while sessions from earlier tests
    // were still connected sent the connected-clients counter below zero on the next disconnect,
    // where it wrapped to 18446744073709551615 and every later assertion about it was nonsense.
    auto*      stats = server()->systemStatistics();
    const auto initialReceived = stats->receivedCounters();
    const auto initialSent = stats->sentCounters();

    auto [subscriber, publisher, _] = createTestSubscriberAndPublisher();
    this_thread::sleep_for(10ms);

    const auto received = stats->receivedCounters();
    const auto sent = stats->sentCounters();
    EXPECT_EQ(2U, received.anyMessages - initialReceived.anyMessages);
    EXPECT_EQ(0U, received.publishMessages - initialReceived.publishMessages);
    EXPECT_EQ(2U, sent.anyMessages - initialSent.anyMessages);
    EXPECT_EQ(0U, sent.publishMessages - initialSent.publishMessages);
    subscriber->disconnect();
    publisher->disconnect();
}

TEST_F(XMQ_SysTopicsTests, SendReceive_Messages_Subscribe)
{
    using enum SystemStatistics::SysTopicKind;

    auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    this_thread::sleep_for(10ms);

    auto*      stats = server()->systemStatistics();
    const auto initialReceived = stats->receivedCounters();
    const auto initialSent = stats->sentCounters();

    // Received Subscribe, sent SubscribeAck
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
    this_thread::sleep_for(10ms);

    const auto received = stats->receivedCounters();
    const auto sent = stats->sentCounters();
    EXPECT_EQ(1U, received.anyMessages - initialReceived.anyMessages);
    EXPECT_EQ(0U, received.publishMessages - initialReceived.publishMessages);
    EXPECT_EQ(1U, sent.anyMessages - initialSent.anyMessages);
    EXPECT_EQ(0U, sent.publishMessages - initialSent.publishMessages);
}

/**
 * Test the message and publish message send and receive counters
 */
TEST_F(XMQ_SysTopicsTests, SendReceive_Messages_PublishQOS1)
{
    using enum SystemStatistics::SysTopicKind;

    auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
    this_thread::sleep_for(10ms);

    auto*      stats = server()->systemStatistics();
    const auto initialReceived = stats->receivedCounters();
    const auto initialSent = stats->sentCounters();

    publisher->publish(topicName, "message", Qos::Qos1);
    this_thread::sleep_for(10ms);

    const auto received = stats->receivedCounters();
    const auto sent = stats->sentCounters();
    // Publisher connection, server:
    // - Received Publish, sent PublishAck
    // Subscriber connection, server:
    // - Sent Publish, received PublishAck
    EXPECT_EQ(2U, received.anyMessages - initialReceived.anyMessages);
    EXPECT_EQ(1U, received.publishMessages - initialReceived.publishMessages);
    EXPECT_EQ(2U, sent.anyMessages - initialSent.anyMessages);
    EXPECT_EQ(1U, sent.publishMessages - initialSent.publishMessages);
}

/**
 * Test the message and publish message send and receive counters
 */
TEST_F(XMQ_SysTopicsTests, SendReceive_Messages_PublishQOS2)
{
    using enum SystemStatistics::SysTopicKind;

    auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
    this_thread::sleep_for(10ms);

    auto*      stats = server()->systemStatistics();
    const auto initialReceived = stats->receivedCounters();
    const auto initialSent = stats->sentCounters();

    publisher->publish(topicName, "message", Qos::Qos2);
    this_thread::sleep_for(10ms);

    const auto received = stats->receivedCounters();
    const auto sent = stats->sentCounters();
    // Publisher connection, server:
    // - Received Publish, sent PublishReceive
    // - Received PublishRelease, sent PublishComplete
    // Subscriber connection, server:
    // - Sent Publish, received PublishAck
    EXPECT_EQ(3U, received.anyMessages - initialReceived.anyMessages);
    EXPECT_EQ(1U, received.publishMessages - initialReceived.publishMessages);
    EXPECT_EQ(3U, sent.anyMessages - initialSent.anyMessages);
    EXPECT_EQ(1U, sent.publishMessages - initialSent.publishMessages);
}

TEST_F(XMQ_SysTopicsTests, SendReceive_Messages_PublishRetain)
{
    using enum SystemStatistics::SysTopicKind;

    auto [subscriber, publisher, topicName] = createTestSubscriberAndPublisher();
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
    this_thread::sleep_for(100ms);

    const auto* stats = server()->systemStatistics();

    const auto topic = client::MqttClient::getTopic(topicName);

    // This counter is how many messages the whole broker is holding retained, and other tests leave
    // theirs behind - so what this one can say is that it added one and then took it away again.
    const auto initialRetained = stats->getValue(BrokerMessagesRetainedCount);

    publisher->publish(topic, Buffer("message"), Qos::Qos1, {}, true);
    publisher->publish(topic, Buffer("message"), Qos::Qos1);

    EXPECT_EQ(initialRetained + 1,
              waitForCounter(BrokerMessagesRetainedCount, initialRetained + 1));

    // An empty retained payload removes it again.
    publisher->publish(topic, Buffer(""), Qos::Qos1, {}, true);

    EXPECT_EQ(initialRetained, waitForCounter(BrokerMessagesRetainedCount, initialRetained));
}
