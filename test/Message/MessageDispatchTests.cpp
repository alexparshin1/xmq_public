/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/
#include "base/MessageDispatch.h"
#include "common/mqtt/PublishMessage.h"
//#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace std;
using namespace xmq;
using namespace testing;

TEST(MessageDispatchTests, DefaultConstructor)
{
    const MessageDispatch dispatch;
    EXPECT_EQ(dispatch.getPublishMessage(), nullptr);
    EXPECT_EQ(dispatch.m_deliveryId, 0);
    EXPECT_EQ(dispatch.m_flags.getQos(), Qos::Qos0);
    EXPECT_EQ(dispatch.m_flags.isDuplicate(), false);
    EXPECT_EQ(dispatch.m_flags.isRetain(), false);
}

TEST(MessageDispatchTests, Constructor1)
{
    TopicManager topicManager;
    const string payload("Test payload");
    const auto*  topic = topicManager.getTopic("test/topic");
    auto         message = std::make_shared<mqtt::PublishMessage>(topic, payload);

    MessageDispatch dispatch1(message, Qos::Qos1, 123, true, true);
    EXPECT_EQ(dispatch1.getPublishMessage(), message);
    EXPECT_EQ(dispatch1.m_deliveryId, 123);
    EXPECT_EQ(dispatch1.m_flags.getQos(), Qos::Qos1);
    EXPECT_EQ(dispatch1.m_flags.isDuplicate(), true);
    EXPECT_EQ(dispatch1.m_flags.isRetain(), true);

    MessageDispatch dispatch2(message, Qos::Qos2, 124, false, false);
    EXPECT_EQ(dispatch2.getPublishMessage(), message);
    EXPECT_EQ(dispatch2.m_deliveryId, 124);
    EXPECT_EQ(dispatch2.m_flags.getQos(), Qos::Qos2);
    EXPECT_EQ(dispatch2.m_flags.isDuplicate(), false);
    EXPECT_EQ(dispatch2.m_flags.isRetain(), false);
}

TEST(MessageDispatchTests, Constructor2)
{
    TopicManager topicManager;
    const string payload("Test payload");
    const auto*  topic = topicManager.getTopic("test/topic");
    auto         message = std::make_shared<mqtt::PublishMessage>(topic, payload);

    MessageDispatch dispatch1(message, Qos::Qos1, 123, true, {100, 101});
    EXPECT_EQ(dispatch1.getPublishMessage(), message);
    EXPECT_EQ(dispatch1.m_deliveryId, 123);
    EXPECT_EQ(dispatch1.m_flags.getQos(), Qos::Qos1);
    EXPECT_EQ(dispatch1.m_flags.isRetain(), true);
    EXPECT_EQ(dispatch1.m_subscriptionIds, SubscriptionIdSet({100, 101}));

    MessageDispatch dispatch2(message, Qos::Qos2, 124, false, {102, 103});
    EXPECT_EQ(dispatch2.getPublishMessage(), message);
    EXPECT_EQ(dispatch2.m_deliveryId, 124);
    EXPECT_EQ(dispatch2.m_flags.getQos(), Qos::Qos2);
    EXPECT_EQ(dispatch2.m_flags.isDuplicate(), false);
    EXPECT_EQ(dispatch2.m_flags.isRetain(), false);
    EXPECT_EQ(dispatch2.m_subscriptionIds, SubscriptionIdSet({102, 103}));
}
