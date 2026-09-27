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

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

void setRetainMessage(const client::SMqttClient& publisher, const string& topic, const string& testPayload, Semaphore& publishReceived, const string& publishPayload)
{
    COUT_TS("Set retain [" << testPayload << "] on topic " << topic);

    // Send a message 'This is a test' with isRetain flag
    publisher->publish(topic, testPayload, Qos::Qos0, true);

    // Wait for the subscriber to receive that message. Now the isRetain is set and we can disconnect subscriber.
    EXPECT_TRUE(publishReceived.wait_for(1000s));
    EXPECT_STREQ(testPayload.c_str(), publishPayload.c_str());
    this_thread::sleep_for(XMQ_ServerTests::TinyTimeout);
}

string makeSubscribeTopic(const string& baseTopicName, const RetainTestMode testMode)
{
    switch (testMode)
    {
        using enum RetainTestMode;
        case RegularTopic:
            break;
        case TopLevelTopic:
            return String(baseTopicName).replace("/", "-").c_str();
        case RegularWildcard:
            return "topic/+";
        case TopLevelWildcard:
            return "#";
    }
    return baseTopicName;
}

void reconnectClients(const ProtocolVersion      protocolVersion,
                      const client::SMqttClient& publisher,
                      const client::SMqttClient& subscriber,
                      const RetainTestMode       testMode,
                      const string&              publisherClientId,
                      const string&              subscriberClientId,
                      const string&              topicName)
{
    const string subscribeToTopic = makeSubscribeTopic(topicName, testMode);

    // Connect two clients: publisher and subscriber
    if (subscriber)
    {
        ConnectCredentials credentials {subscriberClientId, "user", "secret"};
        subscriber->connect(Host("localhost", ServerTests_Suite::TestTcpPortNumber), credentials, {.m_cleanSession = true}, protocolVersion);

        COUT_TS("Subscribe to topic " << subscribeToTopic);
        subscriber->subscribe(subscribeToTopic);
        this_thread::sleep_for(100ms);
    }

    if (publisher)
    {
        ConnectCredentials credentials {publisherClientId, "user", "secret"};
        publisher->connect(Host("localhost", ServerTests_Suite::TestTcpPortNumber), credentials, {.m_cleanSession = true}, protocolVersion);
    }
}

tuple<client::SMqttClient, client::SMqttClient, string> connectClients(const ProtocolVersion protocolVersion,
                                                                       string&               publishPayload,
                                                                       Semaphore&            publishReceived,
                                                                       const string&         publisherClientId,
                                                                       const string&         subscriberClientId,
                                                                       const string&         topicName)
{
    // Connect two clients: publisher and subscriber
    const auto subscriber = make_shared<client::MqttClient>();
    const auto publisher = make_shared<client::MqttClient>();

    subscriber->onMessage(
        [&publishReceived, &publishPayload](const SPublishMessage& publishMessage)
        {
            publishPayload.assign(bit_cast<const char*>(publishMessage->payloadData()), publishMessage->payloadSize());
            publishReceived.post();
        });

    reconnectClients(protocolVersion, publisher, subscriber, RetainTestMode::RegularTopic, publisherClientId, subscriberClientId, topicName);

    return {publisher, subscriber, topicName};
}

} // namespace

void XMQ_ServerTests::testRetainMessages(ProtocolVersion protocolVersion, RetainTestMode testMode)
{
    auto logger = debugLog(true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const String testPayload("This is a test");
    String       publishPayload;

    Semaphore publishReceived;
    auto [publisher, subscriber, topic] = connectClients(protocolVersion, publishPayload, publishReceived,
                                                         publisherClientId, subscriberClientId, topicName);

    ASSERT_TRUE(subscriber->isConnected());
    ASSERT_TRUE(publisher->isConnected());

    // Send a message 'This is a test' with isRetain flag
    setRetainMessage(publisher, topic, testPayload, publishReceived, publishPayload);

    this_thread::sleep_for(100ms);

    subscriber->hangup();

    publishPayload.clear();

    this_thread::sleep_for(10ms);

    reconnectClients(protocolVersion, nullptr, subscriber, testMode, publisherClientId, subscriberClientId, topicName);
    ASSERT_TRUE(subscriber->isConnected());

    // No need to subscribe since the session isn't clean
    ASSERT_TRUE(publishReceived.wait_for(1000s));
    EXPECT_STREQ(testPayload.c_str(), publishPayload.c_str());

    // Send a message with the empty payload and isRetain flag
    publisher->publish(topic, string(), Qos::Qos0, true);

    // Wait for the retained message to be cleared.
    this_thread::sleep_for(10ms);

    subscriber->hangup();
    this_thread::sleep_for(100ms);

    // Connect the subscriber and wait until receiving the retained message.
    reconnectClients(protocolVersion, nullptr, subscriber, testMode, publisherClientId, subscriberClientId, topicName);
    ASSERT_TRUE(subscriber->isConnected());
    EXPECT_FALSE(publishReceived.wait_for(SmallTimeout));

    // Send a message with some payload and no isRetain flag
    publisher->publish(topicName, "Some payload", Qos::Qos1);

    // Wait until receiving a message with "Some payload" payload
    EXPECT_TRUE(publishReceived.wait_for(SmallTimeout));
    EXPECT_STREQ("Some payload", publishPayload.c_str());
    this_thread::sleep_for(TinyTimeout);
    subscriber->disconnect();
    this_thread::sleep_for(TinyTimeout);

    // Reconnect the subscriber and wait until timeout receiving the retained message.
    reconnectClients(protocolVersion, nullptr, subscriber, testMode, publisherClientId, subscriberClientId, topicName);
    ASSERT_TRUE(subscriber->isConnected());

    EXPECT_FALSE(publishReceived.wait_for(SmallTimeout));

    publisher->disconnect();
    subscriber->disconnect();
}

/**
 * Test if subscribing to a topic with a retained message delivers the retained message from that topic.
 */
TEST_F(XMQ_ServerTests, Publish_SetRetainSubscribeRegularTopic)
{
    testRetainMessages(ProtocolVersion::MqttV5, RetainTestMode::RegularTopic);
}

/**
 * Test if subscribing to '#' wildcard with a retained message delivers the retained message from the matching topic.
 */
TEST_F(XMQ_ServerTests, Publish_SetRetainSubscribeTopLevelWildcard)
{
    testRetainMessages(ProtocolVersion::MqttV5, RetainTestMode::TopLevelWildcard);
}

/**
 * Test if subscribing to 'topic/#' wildcard with retain message delivers retained message from matching topic.
 */
TEST_F(XMQ_ServerTests, Publish_SetRetainSubscribeRegularWildcard)
{
    testRetainMessages(ProtocolVersion::MqttV5, RetainTestMode::RegularWildcard);
}
