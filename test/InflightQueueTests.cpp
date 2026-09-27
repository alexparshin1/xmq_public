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
