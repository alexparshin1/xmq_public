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

#include "base/MessageQueue.h"
#include "common/mqtt/PublishMessage.h"
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;

using namespace xmq;

namespace {
set<MessageId> queueMessages(const STopicManager& topicManager, MessageQueue& inflightQueue, const size_t expectedMessageCount, Qos qos)
{
    const string   payload("payload");
    set<MessageId> messageIds;

    for (size_t i = 0; i < expectedMessageCount; ++i)
    {
        SMessage message(new mqtt::PublishMessage(topicManager->getTopic("topic1"), string_view(payload)));
        auto     messageDispatch = make_shared<MessageDispatch>(message, qos);
        auto     messageId = inflightQueue.queueMessage(messageDispatch);
        if (qos != Qos::Qos0)
        {
            messageIds.insert(messageId);
        }
    }
    return messageIds;
}

void passMessages(Qos qos, size_t totalMessageCount, uint16_t maxInflightMessages, bool ackAllMessages, bool expectSuccess)
{
    size_t messageCount = 0;

    MessageQueue messageQueue(maxInflightMessages,
                              [&messageCount](const SMessageDispatch& messageDispatch)
                              {
                                  if (messageDispatch->m_message->is(Message::Type::Publish))
                                  {
                                      ++messageCount;
                                  }
                              });

    const auto measurePerformance = totalMessageCount >= 1000;

    auto topicManager = make_shared<TopicManager>();

    Stopwatch stopwatch;

    const auto messageIds = queueMessages(topicManager, messageQueue, totalMessageCount, qos);

    if (ackAllMessages)
    {
        if (qos == Qos::Qos1)
        {
            for (const auto& messageId: messageIds)
            {
                messageQueue.receiveAck(messageId, Message::Type::PublishAck);
            }
        }
        else if (qos == Qos::Qos2)
        {
            for (const auto& messageId: messageIds)
            {
                messageQueue.receiveAck(messageId, Message::Type::PublishReceived);
                messageQueue.receiveAck(messageId, Message::Type::PublishComplete);
            }
        }
    }

    if (expectSuccess)
    {
        EXPECT_EQ(totalMessageCount, messageCount);
    }
    else
    {
        EXPECT_EQ(maxInflightMessages, messageCount);
    }

    stopwatch.stop();

    if (measurePerformance)
    {
        COUT("InflightQueue, getQos" << static_cast<int>(qos) << ": Passed " << totalMessageCount << " msgs, " << setprecision(1) << fixed << totalMessageCount / stopwatch.milliseconds() << "K msgs/s");
    }
}

constexpr uint16_t maxInflightMessages = 10;

} // namespace

// MQTT 5.0 4.4 requires unacknowledged messages to be resent under their original packet ids, so
// that an acknowledgement still in flight when a session moves between nodes matches the message
// it was sent for. Restoring under a fresh id would let a late ack retire an unrelated message.
TEST(XMQ_InflightQueue, restoreKeepsOriginalDeliveryId)
{
    MessageQueue messageQueue(maxInflightMessages, [](const SMessageDispatch&) {});

    const auto topicManager = make_shared<TopicManager>();
    const auto payload = string("payload");

    constexpr MessageId originalId = 700;

    SMessage   message(new mqtt::PublishMessage(topicManager->getTopic("topic1"), string_view(payload)));
    auto       restored = make_shared<MessageDispatch>(message, Qos::Qos1, originalId);
    const auto restoredCopy = restored;

    EXPECT_EQ(originalId, messageQueue.restoreMessage(restored));
    EXPECT_EQ(originalId, restoredCopy->m_deliveryId);

    // The allocator must now sit above the restored id, or a message queued afterwards would be
    // handed an id the client is still expecting to acknowledge.
    SMessage   freshMessage(new mqtt::PublishMessage(topicManager->getTopic("topic1"), string_view(payload)));
    auto       fresh = make_shared<MessageDispatch>(freshMessage, Qos::Qos1);
    const auto freshId = messageQueue.queueMessage(fresh);
    EXPECT_GT(freshId, originalId);
}

// Records written before the delivery id was persisted carry 0, and have to keep working.
TEST(XMQ_InflightQueue, restoreWithoutDeliveryIdAllocatesFresh)
{
    MessageQueue messageQueue(maxInflightMessages, [](const SMessageDispatch&) {});

    const auto topicManager = make_shared<TopicManager>();
    const auto payload = string("payload");

    SMessage message(new mqtt::PublishMessage(topicManager->getTopic("topic1"), string_view(payload)));
    auto     restored = make_shared<MessageDispatch>(message, Qos::Qos1);
    ASSERT_EQ(0, restored->m_deliveryId);

    const auto assignedId = messageQueue.restoreMessage(restored);
    EXPECT_NE(0, assignedId) << "0 is not a valid MQTT packet identifier";
}

// An id allocated before the delivery record is persisted must survive queueing unchanged,
// otherwise the record and the wire would disagree about which message the id refers to.
TEST(XMQ_InflightQueue, queueKeepsPreallocatedDeliveryId)
{
    MessageQueue messageQueue(maxInflightMessages, [](const SMessageDispatch&) {});

    const auto topicManager = make_shared<TopicManager>();
    const auto payload = string("payload");

    const auto preallocatedId = messageQueue.nextDeliveryId();
    EXPECT_NE(0, preallocatedId);

    SMessage   message(new mqtt::PublishMessage(topicManager->getTopic("topic1"), string_view(payload)));
    auto       dispatch = make_shared<MessageDispatch>(message, Qos::Qos1, preallocatedId);
    const auto queuedId = messageQueue.queueMessage(dispatch);

    EXPECT_EQ(preallocatedId, queuedId);
    EXPECT_EQ(preallocatedId, dispatch->m_deliveryId);
}

TEST(XMQ_InflightQueue, passFewMessages)
{
    constexpr size_t totalMessageCount = 10;
    passMessages(Qos::Qos0, totalMessageCount, maxInflightMessages, false, true);
    passMessages(Qos::Qos1, totalMessageCount, maxInflightMessages, false, true);
    passMessages(Qos::Qos2, totalMessageCount, maxInflightMessages, false, true);
}

TEST(XMQ_InflightQueue, passMessagesNoAcks)
{
    constexpr size_t totalMessageCount = 20;
    passMessages(Qos::Qos0, totalMessageCount, maxInflightMessages, false, true);
    passMessages(Qos::Qos1, totalMessageCount, maxInflightMessages, false, false);
    passMessages(Qos::Qos2, totalMessageCount, maxInflightMessages, false, false);
}

TEST(XMQ_InflightQueue, passMessagesWithAcks)
{
    constexpr size_t totalMessageCount = 20;
    passMessages(Qos::Qos0, totalMessageCount, maxInflightMessages, true, true);
    passMessages(Qos::Qos1, totalMessageCount, maxInflightMessages, true, true);
    passMessages(Qos::Qos2, totalMessageCount, maxInflightMessages, true, true);
}

TEST(XMQ_InflightQueue, performance)
{
    constexpr size_t totalMessageCount = 1000;
    passMessages(Qos::Qos0, totalMessageCount, maxInflightMessages, true, true);
    passMessages(Qos::Qos1, totalMessageCount, maxInflightMessages, true, true);
    passMessages(Qos::Qos2, totalMessageCount, maxInflightMessages, true, true);
}

// The in-flight window is the client's own, but what waits behind it is the node's memory: a
// subscriber that stops acknowledging used to grow it without bound, one session at a time. On a
// two-node Windows cluster 3000 subscriptions fanned out to 16 GB of virtual memory on 8 GB
// machines, and both brokers stopped. The queue now drops the newest message instead, and says so.
TEST(XMQ_InflightQueue, queueLimitsDropTheNewestMessageAndCountIt)
{
    size_t forwarded = 0;
    // A window of one: the first message is in flight, everything after it has to wait.
    MessageQueue messageQueue(1,
                              [&forwarded](const SMessageDispatch&)
                              {
                                  ++forwarded;
                              });
    messageQueue.setQueueLimits(2, 0);

    const auto topicManager = make_shared<TopicManager>();
    const auto payload = string("payload");

    const auto queue = [&](const char* topic)
    {
        SMessage message(new mqtt::PublishMessage(topicManager->getTopic(topic), string_view(payload)));
        return messageQueue.queueMessage(make_shared<MessageDispatch>(message, Qos::Qos1));
    };

    EXPECT_NE(0U, queue("topic1"));  // in flight
    EXPECT_NE(0U, queue("topic2"));  // waiting, 1 of 2
    EXPECT_NE(0U, queue("topic3"));  // waiting, 2 of 2
    EXPECT_EQ(0U, queue("topic4"));  // dropped
    EXPECT_EQ(0U, queue("topic5"));  // dropped

    EXPECT_EQ(3U, messageQueue.size()) << "the queue grew past its limit";
    EXPECT_EQ(2U, messageQueue.droppedMessages());
    EXPECT_EQ(1U, forwarded) << "a dropped message was sent anyway";
}

// The same bound in bytes, because a message count says nothing about a queue of 256 KB payloads.
TEST(XMQ_InflightQueue, byteLimitDropsWhatDoesNotFit)
{
    MessageQueue messageQueue(1, [](const SMessageDispatch&) {});
    // Room for one payload and the topic name that goes with it, and not for the second.
    messageQueue.setQueueLimits(0, 20);

    const auto topicManager = make_shared<TopicManager>();
    const auto payload = string("payload"); // 7 bytes, topic1 is 6

    const auto queue = [&](const char* topic)
    {
        SMessage message(new mqtt::PublishMessage(topicManager->getTopic(topic), string_view(payload)));
        return messageQueue.queueMessage(make_shared<MessageDispatch>(message, Qos::Qos1));
    };

    EXPECT_NE(0U, queue("topic1")); // in flight, not counted against the queue
    EXPECT_NE(0U, queue("topic2")); // waiting: 7 bytes of payload and a 6-byte topic
    EXPECT_EQ(0U, queue("topic3")) << "the byte limit was not applied";
    EXPECT_EQ(1U, messageQueue.droppedMessages());
}
