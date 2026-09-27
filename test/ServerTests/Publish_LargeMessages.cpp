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
#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_P(XMQ_ServerTests, Publish_LargeMessages)
{
    auto protocolVersion = GetParam();

    auto publishOnly = false;

    auto logger = debugLog(false);
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    constexpr size_t testMessageCount {2};
    constexpr size_t testPayloadSize {16384};

    Buffer testPayload(testPayloadSize);
    testPayload.append("[");
    while (testPayload.bytes() < testPayloadSize)
    {
        testPayload.append("This is a test. ");
    }

    testPayload.bytes(testPayloadSize - 1);
    testPayload.append("]");

    vector<string> publishPayloads;

    // Connect two clients: publisher and subscriber
    shared_ptr<TestMqttClient> subscriber;
    if (!publishOnly)
    {
        subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, protocolVersion);
        ASSERT_TRUE(subscriber->isConnected());
    }

    Semaphore allPublishAcksReceived;
    size_t    receivedAckCount {0};

    auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion);
    ASSERT_TRUE(publisher->isConnected());
    publisher->onAck(
        [&receivedAckCount, &allPublishAcksReceived](const SMessage& message)
        {
            if (message->is(Message::Type::PublishAck))
            {
                ++receivedAckCount;
                if (receivedAckCount == testMessageCount)
                {
                    allPublishAcksReceived.post();
                }
            }
        });

    if (!publishOnly)
    {
        // Subscribe to topic1
        const Destination destination(client::MqttClient::getTopic(topicName));
        subscriber->subscribe(destination);
        this_thread::sleep_for(100ms);
    }

    Semaphore allPublishMessagesReceived;
    size_t    receivedMessageCount {0};

    subscriber->onMessage(
        [&receivedMessageCount, &publishPayloads, &allPublishMessagesReceived](const SPublishMessage& publishMessage)
        {
            ++receivedMessageCount;
            publishPayloads.emplace_back(publishMessage->payload());
            if (publishPayloads.size() == testMessageCount)
            {
                allPublishMessagesReceived.post();
            }
        });

    publisher->publishMultiple(topicName, testPayload, testMessageCount, Qos::Qos1);

    logger->debug("Waiting for all publish acks to be received");

    EXPECT_TRUE(allPublishAcksReceived.wait_for(3s));

    logger->debug("All publish acks received");

    if (!publishOnly)
    {
        EXPECT_TRUE(allPublishMessagesReceived.wait_for(10s));
        EXPECT_EQ(testMessageCount, receivedMessageCount);
        subscriber->hangup();

        ASSERT_EQ(testMessageCount, receivedMessageCount);

        for (const auto& payload: publishPayloads)
        {
            EXPECT_STREQ(testPayload.c_str(), payload.c_str());
        }
    }

    publisher->disconnect();
}
