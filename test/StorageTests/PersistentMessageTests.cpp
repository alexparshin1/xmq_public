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

#include "StorageTests.h"
#include "TestStorage.h"
#include "server/MessageDelivery.h"

#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

void XMQ_StorageTests::linkMessageTests()
{
    // This method is only defined to force linking of the file
}

TEST_F(XMQ_StorageTests, messagePackUnpack)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);

    ASSERT_TRUE(testStorage->session());
    ASSERT_FALSE(testStorage->session()->isCleanSession());

    sptk::Semaphore completed;
    const auto      messageDelivery = createTestMessageDelivery(server().get(), testStorage->session(),
                                                                "Test Message. Test Message. Test Message. Test Message. Test Message.", [&completed](const std::shared_ptr<MessageDelivery>&)
                                                                {
                                                               completed.post();
                                                           });
    completed.wait();
    const auto publish = dynamic_pointer_cast<PublishMessage>(messageDelivery->m_message);

    const auto sessionKeyId = "session_" + testStorage->session()->getClientId() + "_messages";
    const auto redis = testStorage->storage()->getRedis();
    const auto messagesKeyValues = redis->getHashValues(sessionKeyId);
    ASSERT_EQ(1U, messagesKeyValues.size());

    const auto messageDelivery2 = make_shared<MessageDelivery>(testStorage->session(), messagesKeyValues.begin()->second.asBuffer());
    // QoS and retain survive the round-trip unchanged.
    EXPECT_EQ(messageDelivery->m_flags.getQos(), messageDelivery2->m_flags.getQos());
    EXPECT_EQ(messageDelivery->m_flags.isRetain(), messageDelivery2->m_flags.isRetain());

    // The duplicate flag deliberately does not: a record read back from Redis may already have
    // been sent to the client before the server stopped, and the record carries no "delivery
    // attempted" state to tell. Restored deliveries are always flagged as duplicates, which is
    // the safe direction - sending DUP=0 for a message the client already saw is a protocol
    // violation, while DUP=1 for one it hasn't is explicitly permitted (MQTT 3.1.1 3.3.1.1).
    EXPECT_FALSE(messageDelivery->m_flags.isDuplicate());
    EXPECT_TRUE(messageDelivery2->m_flags.isDuplicate());

    EXPECT_EQ(messageDelivery->m_subscriptionIds, messageDelivery2->m_subscriptionIds);

    const auto publish2 = dynamic_pointer_cast<PublishMessage>(messageDelivery2->m_message);
    EXPECT_EQ(publish->payload(), publish2->payload());
    EXPECT_EQ(publish->destination()->fullName(), publish2->destination()->fullName());
    EXPECT_EQ(publish->getSourceNode(), publish2->getSourceNode());
    EXPECT_EQ(publish->getSender(), publish2->getSender());

    auto properties2 = publish2->getProperties();
    ASSERT_TRUE(properties2);

    int64_t intValue;
    EXPECT_TRUE(properties2->getProperty(Property::MaximumQOS, intValue));
    EXPECT_EQ(intValue, static_cast<int>(Qos::Qos2));

    string_view strValue;
    EXPECT_TRUE(properties2->getProperty(Property::CorrelationData, strValue));
    EXPECT_EQ(strValue, "correlation_data");
}

// The packet id a message was sent under has to survive persistence: after a session moves to
// another node the message is redelivered from its record, and MQTT requires the original id.
TEST_F(XMQ_StorageTests, messagePackUnpackPreservesDeliveryId)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);
    ASSERT_TRUE(testStorage->session());

    constexpr MessageId originalDeliveryId = 4711;

    const auto* topic = server()->getTopic("test/topic");
    auto        publish = make_shared<mqtt::PublishMessage>(topic, string_view("delivery id round trip"));

    sptk::Semaphore completed;
    const auto      messageDelivery = MessageDelivery::create(testStorage->session(), std::move(publish), Qos::Qos1,
                                                              originalDeliveryId, SubscriptionIdSet {}, false, 0,
                                                              [&completed](const shared_ptr<MessageDelivery>&)
                                                              {
                                                             completed.post();
                                                         });
    completed.wait();
    ASSERT_EQ(originalDeliveryId, messageDelivery->m_deliveryId);

    const auto sessionKeyId = "session_" + testStorage->session()->getClientId() + "_messages";
    const auto messagesKeyValues = testStorage->storage()->getRedis()->getHashValues(sessionKeyId);
    ASSERT_FALSE(messagesKeyValues.empty());

    const auto restored = make_shared<MessageDelivery>(testStorage->session(), messagesKeyValues.begin()->second.asBuffer());
    EXPECT_EQ(originalDeliveryId, restored->m_deliveryId)
        << "The packet id an unacknowledged message was sent under did not survive persistence";
}

TEST_F(XMQ_StorageTests, messagePackUnpackPerformance)
{
    constexpr size_t messageCount = 2500;

    const auto testStorage = make_shared<TestStorage>(server(), false);

    sptk::Stopwatch sw;
    sw.start();
    const auto messageDeliveries = createTestMessageDeliveries(messageCount, server().get(), testStorage.get(), 64);
    sw.stop();
    COUT(format("Create {} messages took {:0.1f} ms, {:0.1f}K/s", messageCount, sw.milliseconds(), messageCount / sw.milliseconds()));

    const auto sessionKeyId = "session_" + testStorage->session()->getClientId() + "_messages";
    const auto redis = testStorage->storage()->getRedis();
    const auto messagesKeyValues = redis->getHashValues(sessionKeyId);
    ASSERT_EQ(messageCount, messagesKeyValues.size());

    const auto messageDelivery2 = make_shared<MessageDelivery>(testStorage->session(), messagesKeyValues.begin()->second.asBuffer());
}
