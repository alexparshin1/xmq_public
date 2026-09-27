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

#include "base/AckMessage.h"
#include "base/Message.h"
#include "common/mqtt/PublishMessage.h"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

using namespace std;
using namespace xmq;

class MessageDeliveryTest : public testing::Test
{
protected:
    void SetUp() override
    {
        TopicManager topicManager;
        testMessage = make_shared<mqtt::PublishMessage>(topicManager.getTopic("topic1"), string_view("topic1"));
        testMessage->setId(12345);

        ackMessage = make_shared<AckMessage>(Message::Type::PublishAck);
        ackMessage->setId(12345);
        ackMessage->setReasonCode(ReasonCode::Success);
    }

    void TearDown() override
    {
        testMessage.reset();
        ackMessage.reset();
    }

    shared_ptr<Message>    testMessage;
    shared_ptr<AckMessage> ackMessage;
};

TEST_F(MessageDeliveryTest, basicMessageDelivery)
{
    ASSERT_NE(testMessage, nullptr);
    EXPECT_EQ(testMessage->type(), Message::Type::Publish);
    EXPECT_EQ(testMessage->getId(), 12345);
}

TEST_F(MessageDeliveryTest, acknowledgmentMessageHandling)
{
    ASSERT_NE(ackMessage, nullptr);
    EXPECT_EQ(ackMessage->type(), Message::Type::PublishAck);
    EXPECT_EQ(ackMessage->getId(), 12345);
    EXPECT_EQ(ackMessage->getReasonCode(), ReasonCode::Success);
}

TEST_F(MessageDeliveryTest, differentMessageTypes)
{
    const auto connectMsg = make_shared<Message>(Message::Type::Connect);
    connectMsg->setId(1001);
    EXPECT_EQ(connectMsg->type(), Message::Type::Connect);
    EXPECT_EQ(connectMsg->getId(), 1001);

    const auto subscribeMsg = make_shared<Message>(Message::Type::Subscribe);
    subscribeMsg->setId(1002);
    EXPECT_EQ(subscribeMsg->type(), Message::Type::Subscribe);
    EXPECT_EQ(subscribeMsg->getId(), 1002);

    const auto disconnectMsg = make_shared<Message>(Message::Type::Disconnect);
    disconnectMsg->setId(1003);
    EXPECT_EQ(disconnectMsg->type(), Message::Type::Disconnect);
    EXPECT_EQ(disconnectMsg->getId(), 1003);
}

TEST_F(MessageDeliveryTest, messageDeliveryFailure)
{
    const auto failAck = make_shared<AckMessage>(Message::Type::PublishAck);
    failAck->setId(9999);
    failAck->setReasonCode(ReasonCode::UnspecifiedError);

    EXPECT_EQ(failAck->getReasonCode(), ReasonCode::UnspecifiedError);
    EXPECT_NE(failAck->getReasonCode(), ReasonCode::Success);
}

TEST_F(MessageDeliveryTest, messageIdTracking)
{
    vector<SMessage> messages;

    for (uint16_t i = 1; i <= 10; ++i)
    {
        auto msg = make_shared<Message>(Message::Type::Publish);
        msg->setId(i);
        messages.push_back(msg);
    }

    ASSERT_EQ(messages.size(), 10U);

    for (size_t i = 0; i < messages.size(); ++i)
    {
        EXPECT_EQ(messages[i]->getId(), i + 1);
    }
}

TEST_F(MessageDeliveryTest, concurrentMessageDelivery)
{
    constexpr int               numThreads = 5;
    constexpr int               messagesPerThread = 10;
    vector<thread>              threads;
    vector<shared_ptr<Message>> deliveredMessages;
    mutex                       deliveryMutex;

    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back([&, t]()
                             {
                                 for (int m = 0; m < messagesPerThread; ++m)
                                 {
                                     auto msg = make_shared<Message>(Message::Type::Publish);
                                     msg->setId(static_cast<MessageId>(t * messagesPerThread + m));

                                     const scoped_lock lock(deliveryMutex);
                                     deliveredMessages.push_back(msg);

                                     this_thread::sleep_for(1ms);
                                 }
                             });
    }

    for (auto& thread: threads)
    {
        thread.join();
    }

    EXPECT_EQ(deliveredMessages.size(), static_cast<size_t>(numThreads * messagesPerThread));
}
