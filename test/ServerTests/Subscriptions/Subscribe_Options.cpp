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

#include "common/DisconnectMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTests::testSubscribeOptions(const ProtocolVersion protocolVersion, const TestSubscribeOption testOption)
{
    Semaphore messageIsReceived;

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    shared_ptr<mqtt::PublishMessage> publishMessageReceived;
    client->onMessage([&publishMessageReceived, &messageIsReceived](const SPublishMessage& message)
                      {
                          publishMessageReceived = dynamic_pointer_cast<mqtt::PublishMessage>(message);
                          messageIsReceived.post();
                      });

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    const auto               rc = client->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = true},
                                                  protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    SubscriptionOptions options(Qos::Qos1);
    bool                retainMessage = false;
    switch (testOption)
    {
        case TestSubscribeOption::TestNoLocal:
            options.m_noLocal = true;
            break;
        case TestSubscribeOption::TestRetainAsPublished:
            options.m_retainAsPublished = true;
            retainMessage = true;
            break;
        default:
            break;
    }

    const Destination destination(client::MqttClient::getTopic(topicName), options);
    ASSERT_TRUE(test::subscribeAndWait(client, destination))
        << "The broker did not acknowledge the subscription";

    this_thread::sleep_for(TinyTimeout);

    client->publish(client::MqttClient::getTopic(topicName), Buffer("Test Data"), Qos::Qos1, {}, retainMessage);

    messageIsReceived.wait_for(TinyTimeout);

    if (testOption == TestSubscribeOption::TestNoLocal)
    {
        if (!publishMessageReceived)
        {
            // For MQTT5, the "no local" flag blocks delivery
            if (protocolVersion != ProtocolVersion::MqttV5)
            {
                FAIL() << "Message expected for MQTT 3.x protocol, but is not received.";
            }
        }
        else
        {
            // For MQTT3, the "no local" flag doesn't affect delivery
            if (protocolVersion == ProtocolVersion::MqttV5)
            {
                FAIL() << "Message not expected for MQTT 5 protocol, but is received.";
            }
        }
    }
    else if (testOption == TestSubscribeOption::TestRetainAsPublished)
    {
        if (!publishMessageReceived)
        {
            FAIL() << "Message expected but is not received.";
        }

        // For MQTT3, the "isRetain as published" flag doesn't exist
        if (publishMessageReceived->isRetain())
        {
            if (protocolVersion != ProtocolVersion::MqttV5)
            {
                FAIL() << "The message isRetain flag is set but not expected for MQTT 3.x protocol.";
            }
        }
        else
        {
            if (protocolVersion == ProtocolVersion::MqttV5)
            {
                FAIL() << "The message isRetain flag is not set but expected for MQTT 5 protocol.";
            }
        }
    }
    else
    {
        if (!publishMessageReceived)
        {
            FAIL() << "Message expected but is not received.";
        }
    }

    client->disconnect();
}

TEST_P(XMQ_ServerTests, Subscribe_Minimal)
{
    const auto protocolVersion = GetParam();
    testSubscribeOptions(protocolVersion, TestSubscribeOption::TestNoLocal);
}

TEST_P(XMQ_ServerTests, Subscribe_NoLocal)
{
    const auto protocolVersion = GetParam();
    testSubscribeOptions(protocolVersion, TestSubscribeOption::TestNoLocal);
}

TEST_P(XMQ_ServerTests, Subscribe_RetainAsPublished)
{
    const auto protocolVersion = GetParam();
    testSubscribeOptions(protocolVersion, TestSubscribeOption::TestRetainAsPublished);
}

TEST_P(XMQ_ServerTests, Subscribe_RetainHandling_RetainAlways)
{
    const auto protocolVersion = GetParam();
    testSubscribeRetainHandling(protocolVersion, SubscribeRetainHandling::RetainAlways);
}

TEST_P(XMQ_ServerTests, Subscribe_RetainHandling_RetainIfNew)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }
    testSubscribeRetainHandling(protocolVersion, SubscribeRetainHandling::RetainIfNew);
}

TEST_P(XMQ_ServerTests, Subscribe_RetainHandling_RetainNever)
{
    const auto protocolVersion = GetParam();
    if (protocolVersion != ProtocolVersion::MqttV5)
    {
        return;
    }
    testSubscribeRetainHandling(protocolVersion, SubscribeRetainHandling::DoNotRetain);
}

void XMQ_ServerTests::testSubscribeRetainHandling(ProtocolVersion protocolVersion, SubscribeRetainHandling retainHandling)
{
    Semaphore messageIsReceived;

    const auto subscriber = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    SPublishMessage publishMessageReceived;
    subscriber->onMessage([&publishMessageReceived, &messageIsReceived](const SPublishMessage& message)
                          {
                              if (message->is(Message::Type::Publish))
                              {
                                  publishMessageReceived = message;
                                  messageIsReceived.post();
                              }
                          });

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    auto                     rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = false}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    const SubscriptionOptions options(Qos::Qos1, retainHandling, true);
    const Destination         destination(client::MqttClient::getTopic(topicName), options);

    ASSERT_TRUE(test::subscribeAndWait(subscriber, destination))
        << "The broker did not acknowledge the subscription";
    this_thread::sleep_for(10ms);

    // Send isRetain message: Retain is set to "Test Data".
    const auto               publisher = make_shared<client::MqttClient>();
    const ConnectCredentials credentials2 {publisherClientId, "user", "secret"};
    rc = publisher->connect(Host("localhost", TestTcpPortNumber), credentials2, {.m_cleanSession = true}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("Test Data"), Qos::Qos0, {}, true);

    this_thread::sleep_for(100ms);

    publisher->disconnect();

    this_thread::sleep_for(SmallTimeout);

    // Receive the message as a regular message
    EXPECT_TRUE(messageIsReceived.wait_for(MediumTimeout));
    subscriber->disconnect();
    this_thread::sleep_for(SmallTimeout);

    rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials,
                             {.m_cleanSession = false}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    // Receive the message as a retained message.
    bool const retainedMessageReceived = messageIsReceived.wait_for(MediumTimeout);

    switch (retainHandling)
    {
        case SubscribeRetainHandling::RetainAlways:
            if (!retainedMessageReceived)
            {
                FAIL() << "Message expected but is not received.";
            }
            break;
        case SubscribeRetainHandling::RetainIfNew:
            if (!retainedMessageReceived)
            {
                FAIL() << "Message expected but is not received.";
            }
            break;
        case SubscribeRetainHandling::DoNotRetain:
            if (retainedMessageReceived && protocolVersion == ProtocolVersion::MqttV5)
            {
                FAIL() << "Message not expected but received.";
            }
            break;
    }

    subscriber->disconnect();
    this_thread::sleep_for(SmallTimeout);

    if (retainHandling == SubscribeRetainHandling::RetainIfNew)
    {
        rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials,
                                 {.m_cleanSession = false}, protocolVersion);
        EXPECT_EQ(ReasonCode::Success, rc);
        EXPECT_FALSE(messageIsReceived.wait_for(SmallTimeout));
        subscriber->disconnect();
    }
}
