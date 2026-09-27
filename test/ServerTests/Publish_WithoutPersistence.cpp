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

// Delivery to a persistent (clean_session false) session on a server running with
// persistence disabled - no Redis at all.
//
// This combination used to lose every QoS1/QoS2 message: MessageDelivery::create()
// blocks on a semaphore that storeRecordAsync() posts from its completion callback, and
// storeRecordAsync() returned early without running the callback when there was no Redis
// connection. The publisher still got its PUBACK, so the loss was silent.
//
// The rest of the server test suite always runs with persistence enabled, so nothing else
// covers this path.

#include "client/MqttClient.h"
#include "test/ServerTests_Suite.h"
#include "test/SubscribeAndWait.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

class XMQ_NoPersistenceTests
    : public ServerTests_Suite
{
protected:
    void SetUp() override
    {
        ServerTests_Suite::SetUp();
        // Replace the suite's default (persistent) server with an in-memory one.
        stopServers();
        m_server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true,
                                "primary", "user", "secret", LogPriority::Info, false);
        this_thread::sleep_for(100ms);
    }

    SServer m_server;
};

shared_ptr<client::MqttClient> connectClient(const string&                 clientId,
                                             const bool                    cleanSession,
                                             const PublishMessageCallback& messageCallback = nullptr)
{
    auto client = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = cleanSession;

    if (messageCallback)
    {
        client->onMessage(messageCallback);
    }

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", ServerTests_Suite::TestTcpPortNumber),
                  ConnectCredentials(clientId, "user", "secret"),
                  connectParameters, ProtocolVersion::MqttV31));

    return client;
}

// Subscribe, publish one message to the same topic from a second client, and report
// whether it was delivered within the timeout.
void expectDelivery(const bool cleanSession, const Qos qos)
{
    const string topicName = format("no-persistence/qos{}/{}", static_cast<int>(qos), cleanSession ? "clean" : "persistent");

    Semaphore  received;
    const auto subscriber = connectClient("subscriber-" + topicName, cleanSession,
                                          [&received](const SPublishMessage&)
                                          {
                                              received.post();
                                          });
    ASSERT_TRUE(test::subscribeAndWait(subscriber, topicName, qos))
        << "The broker did not acknowledge the subscription, so a publish now would measure that "
           "race rather than delivery without persistence";

    const auto publisher = connectClient("publisher-" + topicName, true);
    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("payload"), qos);

    constexpr auto deliveryTimeout = 2000ms;
    EXPECT_TRUE(received.wait_for(deliveryTimeout))
        << "No delivery of a QoS" << static_cast<int>(qos) << " message to a "
        << (cleanSession ? "clean" : "persistent") << " session with persistence disabled";

    publisher->disconnect();
    subscriber->disconnect();
}

} // namespace

TEST_F(XMQ_NoPersistenceTests, Publish_Qos1ToPersistentSession)
{
    expectDelivery(false, Qos::Qos1);
}

TEST_F(XMQ_NoPersistenceTests, Publish_Qos2ToPersistentSession)
{
    expectDelivery(false, Qos::Qos2);
}

TEST_F(XMQ_NoPersistenceTests, Publish_Qos1ToCleanSession)
{
    expectDelivery(true, Qos::Qos1);
}

TEST_F(XMQ_NoPersistenceTests, Publish_Qos2ToCleanSession)
{
    expectDelivery(true, Qos::Qos2);
}

// With no store, no bridge and no cluster node, a message is delivered on the thread that received it.
TEST_F(XMQ_NoPersistenceTests, DeliversOnTheReceiveThread)
{
    EXPECT_TRUE(m_server->deliversOnReceiveThread());
}

// One publisher's messages reach a subscriber in the order they were sent.
TEST_F(XMQ_NoPersistenceTests, Publish_KeepsOrderOfOnePublisher)
{
    constexpr size_t messageCount = 300;
    const string     topicName = "no-persistence/order";

    mutex          receivedMutex;
    vector<string> receivedPayloads;
    Semaphore      allReceived;

    const auto subscriber = connectClient("subscriber-order", true,
                                          [&](const SPublishMessage& message)
                                          {
                                              const scoped_lock lock(receivedMutex);
                                              receivedPayloads.emplace_back(message->payload());
                                              if (receivedPayloads.size() == messageCount)
                                              {
                                                  allReceived.post();
                                              }
                                          });
    ASSERT_TRUE(test::subscribeAndWait(subscriber, topicName, Qos::Qos1));

    const auto publisher = connectClient("publisher-order", true);
    for (size_t index = 0; index < messageCount; ++index)
    {
        publisher->publish(client::MqttClient::getTopic(topicName), Buffer(to_string(index)), Qos::Qos1);
    }

    ASSERT_TRUE(allReceived.wait_for(10000ms));

    const scoped_lock lock(receivedMutex);
    ASSERT_EQ(messageCount, receivedPayloads.size());
    for (size_t index = 0; index < messageCount; ++index)
    {
        EXPECT_EQ(to_string(index), receivedPayloads[index]) << "message " << index << " arrived out of order";
    }

    publisher->disconnect();
    subscriber->disconnect();
}
