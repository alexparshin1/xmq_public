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

#include "common/SubscribeMessage.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;
using namespace sptk;

/**
 * Verify that the default ctor works correctly.
 */
TEST(SubscribeMessageTests, DefaultConstructor)
{
    const SubscribeMessage msg;
    EXPECT_TRUE(msg.getDestinations().empty());
    EXPECT_EQ(msg.getQos(), Qos::AtLeastOnce);
}

/*
 * Verify that the constructor with parameters works correctly.
 */
TEST(SubscribeMessageTests, ConstructorWithParameters)
{
    TopicManager      topicManager;
    constexpr auto    qos = Qos::AtLeastOnce;
    const Destination destination(topicManager.getTopic("test/topic"), SubscriptionOptions {qos});

    const SubscribeMessage msg({destination});
    EXPECT_EQ(msg.getDestinations()[0].m_topic, destination.m_topic);
    EXPECT_EQ(msg.getDestinations()[0].m_subscribeOptions.getQos(), Qos::AtLeastOnce);
    EXPECT_EQ(msg.getQos(), Qos::AtLeastOnce);
}

/**
 * Verify that destinations can be set.
 */
TEST(SubscribeMessageTests, SetterMethods)
{
    TopicManager      topicManager;
    constexpr auto    qos = Qos::AtMostOnce;
    const Destination destination(topicManager.getTopic("home/livingroom/light"), SubscriptionOptions {qos});

    SubscribeMessage msg;

    msg.setDestinations({destination});
    msg.getDestinations()[0].m_subscribeOptions.setQos(qos);

    EXPECT_FALSE(msg.getDestinations().empty());
    EXPECT_EQ(msg.getDestinations()[0].m_topic->fullName(), "home/livingroom/light");
    EXPECT_EQ(msg.getDestinations()[0].m_subscribeOptions.getQos(), qos);
    EXPECT_EQ(msg.getQos(), Qos::AtLeastOnce);
}

/**
 * Verify that the move constructor and assignment operator work correctly.
 */
TEST(SubscribeMessageTests, MoveConstructorAndAssignment)
{
    TopicManager      topicManager;
    const std::string topic("home/livingroom/light");
    constexpr auto    qos = Qos::ExactlyOnce;
    const Destination destination(topicManager.getTopic(topic), SubscriptionOptions {qos});

    SubscribeMessage original({destination});
    SubscribeMessage moved(std::move(original));

    EXPECT_FALSE(moved.getDestinations().empty());
    EXPECT_EQ(moved.getDestinations()[0].m_topic->fullName(), topic);
    EXPECT_EQ(moved.getDestinations()[0].m_subscribeOptions.getQos(), qos);

    SubscribeMessage original2({destination});
    SubscribeMessage moveAssigned;
    moveAssigned = std::move(original2);

    EXPECT_FALSE(moveAssigned.getDestinations().empty());
    EXPECT_EQ(moveAssigned.getDestinations()[0].m_topic->fullName(), topic);
    EXPECT_EQ(moveAssigned.getDestinations()[0].m_subscribeOptions.getQos(), qos);
    EXPECT_EQ(moveAssigned.getQos(), Qos::AtLeastOnce);
}

/**
 * Verify that topic edge cases are handled correctly.
 */
TEST(SubscribeMessageTests, EdgeCases)
{
    TopicManager     topicManager;
    string           topic;
    constexpr auto   qos = Qos::ExactlyOnce;
    Destination      destination(topicManager.getTopic(topic), SubscriptionOptions {qos});
    SubscribeMessage emptyTopic({destination});
    EXPECT_TRUE(emptyTopic.getDestinations()[0].m_topic->fullName().empty());

    topic = std::string(65535, 'a');
    Destination      destination2(topicManager.getTopic(topic), SubscriptionOptions {qos});
    SubscribeMessage longTopic({destination2});
    EXPECT_EQ(longTopic.getDestinations()[0].m_topic->fullName().length(), 65535U);

    topic = "test/+/topic/#";
    Destination      destination3(topicManager.getTopic(topic), SubscriptionOptions {qos});
    SubscribeMessage specialChars({destination3});
    EXPECT_EQ(specialChars.getDestinations()[0].m_topic->fullName(), "test/+/topic/#");
}
