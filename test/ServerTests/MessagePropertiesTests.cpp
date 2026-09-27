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

#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace mqtt;

void XMQ_ServerTestsLink::linkMessagePropertiesTests()
{
    // Force linking this module
}

TEST_P(XMQ_ServerTests, MessageProperties_EncodeDecode)
{
    if (auto protocolVersion = GetParam();
        protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    MessageProperties     properties;
    UserMessageProperties userProperties;
    userProperties["key1"] = "value1";
    userProperties["key2"] = "value2";

    constexpr auto testMaximumPacketSize = 1024;
    constexpr auto testSessionExpiryInterval = 10000;
    constexpr auto testTopicAliasMaximum = 100;

    using enum Property;
    properties.setProperty(AuthenticationMethod, "method");
    properties.setProperty(AuthenticationData, "test");
    properties.setProperty(MaximumPacketSize, testMaximumPacketSize);
    properties.setProperty(SessionExpiryInterval, testSessionExpiryInterval);
    properties.setProperty(TopicAliasMaximum, testTopicAliasMaximum);
    properties.setProperty(RequestProblemInformation, 1);
    properties.setProperty(RequestResponseInformation, 1);
    properties.setUserProperties(userProperties);

    constexpr auto testId123 = 123;
    constexpr auto testId12345 = 12345;
    constexpr auto testId1234567 = 1234567;
    set<uint32_t>  subscriptionIds = {testId123, testId12345, testId1234567};
    properties.setSubscriptionIds(subscriptionIds);

    Buffer buffer(properties.expectedSize());
    auto*  tail = buffer.data();
    properties.write(tail, {});
    auto actualSize = tail - buffer.data();
    buffer.bytes(actualSize);

    EXPECT_EQ(properties.expectedSize(), actualSize);

    MessageProperties properties2;
    properties2.read(buffer.data(), buffer.size());

    string_view stringValue;
    EXPECT_TRUE(properties2.getProperty(AuthenticationMethod, stringValue));
    EXPECT_EQ("method", stringValue);

    string_view binaryValue;
    EXPECT_TRUE(properties2.getProperty(AuthenticationData, binaryValue));
    EXPECT_EQ("test", binaryValue);

    int64_t value {0};
    EXPECT_TRUE(properties2.getProperty(MaximumPacketSize, value));
    EXPECT_EQ(testMaximumPacketSize, value);

    EXPECT_TRUE(properties2.getProperty(SessionExpiryInterval, value));
    EXPECT_EQ(testSessionExpiryInterval, value);

    EXPECT_TRUE(properties2.getProperty(TopicAliasMaximum, value));
    EXPECT_EQ(testTopicAliasMaximum, value);

    EXPECT_TRUE(properties2.getProperty(RequestProblemInformation, value));
    EXPECT_EQ(1, value);

    EXPECT_TRUE(properties2.getProperty(RequestResponseInformation, value));
    EXPECT_EQ(1, value);

    userProperties = properties2.getUserProperties();
    EXPECT_STREQ("value1", userProperties["key1"].c_str());
    EXPECT_STREQ("value2", userProperties["key2"].c_str());

    EXPECT_EQ(subscriptionIds, properties2.getSubscriptionIds());
}

TEST_P(XMQ_ServerTests, MessageProperties_ConnectPerformance)
{
    if (auto protocolVersion = GetParam();
        protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }

    constexpr size_t iterations = 10000;
    constexpr auto   testMaximumPacketSize = 1024;
    constexpr auto   testSessionExpiryInterval = 10000;
    constexpr auto   testTopicAliasMaximum = 100;

    MessageProperties properties;

    using enum Property;
    properties.setProperty(AuthenticationMethod, "method");
    properties.setProperty(AuthenticationData, "test");
    properties.setProperty(MaximumPacketSize, testMaximumPacketSize);
    properties.setProperty(SessionExpiryInterval, testSessionExpiryInterval);
    properties.setProperty(TopicAliasMaximum, testTopicAliasMaximum);
    properties.setProperty(RequestProblemInformation, 1);
    properties.setProperty(RequestResponseInformation, 1);

    // Attempt to set a single property.
    properties.setUserProperty("key0", "value0");

    // Overwrite all properties.
    UserMessageProperties userProperties;
    userProperties["key1"] = "value1";
    userProperties["key2"] = "value2";
    properties.setUserProperties(userProperties);

    Buffer buffer(properties.expectedSize());

    Stopwatch stopwatch;
    stopwatch.start();
    for (size_t i = 0; i < iterations; i++)
    {
        auto* tail = buffer.data();
        properties.write(tail, {});
    }
    stopwatch.stop();

    constexpr auto oneThousand = 1000.0;
    COUT("Encoded " << fixed << setprecision(1) << iterations << " message properties in "
                    << stopwatch.milliseconds() << " ms: " << iterations / stopwatch.seconds() / oneThousand << "K messages per second");

    stopwatch.start();
    for (size_t i = 0; i < iterations; i++)
    {
        MessageProperties properties2;
        properties2.read(buffer.data(), buffer.size());
    }
    stopwatch.stop();
    COUT("Decoded " << fixed << setprecision(1) << iterations << " message properties in "
                    << stopwatch.milliseconds() << " ms: " << iterations / stopwatch.seconds() / oneThousand << "K messages per second");
}

// Test of the Request-Response scenario.
// 1. The responder subscribes to a main topic, expecting the "Hello" message.
// 2. The requester subscribes to the custom ResponseTopic, expecting response "World" to arrive there.
// 3. The requester sends a message "Hello" to the main topic, with the custom ResponseTopic and CorrelationData.
// 4. When the responder receives the "Hello" message, it replies with the "World" message to the ResponseTopic.
// 5. The test ends when the requester gets the "World" message in the ResponseTopic.
TEST_F(XMQ_ServerTests, MessageProperties_RequestResponse)
{
    auto logger = debugLog(true);
    const auto [publisherClientId, responderClientId, topicName] = makeTestNames();
    constexpr auto protocolVersion = ProtocolVersion::MqttV5;

    // Connect two clients: publisher and responder
    const auto responder = make_shared<TestMqttClient>(logEngine(), responderClientId, true, false, protocolVersion);
    ASSERT_TRUE(responder->isConnected());

    Semaphore requestReceived;
    Semaphore responseReceived;

    const auto requester = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion);
    ASSERT_TRUE(requester->isConnected());

    using enum Property;
    auto responseTopicName = topicName + "/response";
    auto correlationData = "12345";

    requester->onMessage(
        [&responseReceived](const xmq::SPublishMessage& responseMessage)
        {
            COUT("Response received: " << responseMessage);
            ASSERT_TRUE(responseMessage->payload() == "World");
            responseReceived.post();
        });

    requester->subscribe(responseTopicName);
    this_thread::sleep_for(100ms);

    responder->onMessage(
        [&requestReceived, &responseTopicName, &correlationData](const xmq::SPublishMessage& requestMessage)
        {
            COUT("Request received: " << requestMessage);
            ASSERT_TRUE(requestMessage->payload() == "Hello");

            string_view gotResponseTopicName;
            string_view gotCorrelationData;

            requestMessage->getProperties()->getProperty(ResponseTopic, gotResponseTopicName);
            ASSERT_EQ(responseTopicName, gotResponseTopicName);

            requestMessage->getProperties()->getProperty(CorrelationData, gotCorrelationData);
            ASSERT_EQ(correlationData, gotCorrelationData);

            requestReceived.post();
        });

    responder->subscribe(topicName);
    this_thread::sleep_for(100ms);

    SMessageProperties properties = make_shared<MessageProperties>();
    properties->setProperty(ResponseTopic, responseTopicName);
    properties->setProperty(CorrelationData, correlationData);

    requester->publish(server()->getTopic(topicName), Buffer("Hello"), Qos::Qos1, properties);
    ASSERT_TRUE(requestReceived.wait_for(500ms));

    responder->publish(server()->getTopic(responseTopicName), Buffer("World"), Qos::Qos1);
    ASSERT_TRUE(responseReceived.wait_for(500ms));
}
