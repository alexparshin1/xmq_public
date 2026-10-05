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
    const auto subscriber = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    mutex          receivedMutex;
    vector<string> receivedPayloads;
    Semaphore      messageIsReceived;
    subscriber->onMessage([&](const SPublishMessage& message)
                          {
                              if (message->is(Message::Type::Publish))
                              {
                                  {
                                      const scoped_lock lock(receivedMutex);
                                      receivedPayloads.emplace_back(bit_cast<const char*>(message->payloadData()),
                                                                    message->payloadSize());
                                  }
                                  messageIsReceived.post();
                              }
                          });
    const auto takeReceived = [&]
    {
        const scoped_lock lock(receivedMutex);
        return exchange(receivedPayloads, {});
    };

    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    auto                     rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials, {.m_cleanSession = false}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    const SubscriptionOptions options(Qos::Qos1, retainHandling, true);
    const Destination         destination(client::MqttClient::getTopic(topicName), options);

    ASSERT_TRUE(test::subscribeAndWait(subscriber, destination))
        << "The broker did not acknowledge the subscription";

    // Send isRetain message: Retain is set to "Test Data".
    const auto               publisher = make_shared<client::MqttClient>();
    const ConnectCredentials credentials2 {publisherClientId, "user", "secret"};
    rc = publisher->connect(Host("localhost", TestTcpPortNumber), credentials2, {.m_cleanSession = true}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("Test Data"), Qos::Qos0, {}, true);

    // Receive the message as a regular message
    EXPECT_TRUE(messageIsReceived.wait_for(MediumTimeout));
    EXPECT_EQ((vector<string> {"Test Data"}), takeReceived());
    subscriber->disconnect();

    // What should not arrive is checked with a marker rather than by waiting for nothing: a
    // retained message is queued to the session before anything published after it - on resume, or
    // right behind the SUBACK - so the marker coming first shows there was none.
    const auto expectOnly = [&](const string& marker, const vector<string>& expected, const string& what)
    {
        publisher->publish(client::MqttClient::getTopic(topicName), Buffer(marker), Qos::Qos0, {}, false);
        const auto deadline = chrono::steady_clock::now() + MediumTimeout;
        vector<string> received;
        while (find(received.begin(), received.end(), marker) == received.end() && chrono::steady_clock::now() < deadline)
        {
            messageIsReceived.wait_for(50ms);
            const auto more = takeReceived();
            received.insert(received.end(), more.begin(), more.end());
        }
        EXPECT_EQ(expected, received) << what;
    };

    rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials,
                             {.m_cleanSession = false}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    // The resumed session has its subscription back without a SUBSCRIBE, and MQTT sends retained
    // messages only in answer to one - whatever the retain handling.
    expectOnly("after resume", {"after resume"}, "A retained message was re-sent on session resume");

    // SUBSCRIBE again, to the subscription the session still holds. Retain handling is about exactly
    // this moment: 0 sends the retained message, 1 sends it only for a subscription that did not
    // exist yet - and this one did - and 2 never sends it.
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destination))
        << "The broker did not acknowledge the repeated subscription";

    if (retainHandling == SubscribeRetainHandling::RetainAlways)
    {
        expectOnly("after subscribe", {"Test Data", "after subscribe"}, "Retain handling 0: no retained message on SUBSCRIBE");
    }
    else
    {
        expectOnly("after subscribe", {"after subscribe"},
                   "Retain handling " + to_string(static_cast<int>(retainHandling)) +
                       ": retained message sent for an existing subscription");
    }

    publisher->disconnect();
    subscriber->disconnect();
}
