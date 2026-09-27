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

TEST_F(XMQ_ServerTests, SharedSubscription_SubscribeToTopicInternally)
{
    const auto storage = server()->getStorage();
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto subscriptionManager = server()->getSubscriptionManager();

    // Create several client sessions and subscribe them to the same shared queue
    constexpr auto         clientCount {3};
    vector<SClientSession> clientSessions;
    const auto*            sharedTopic = client::MqttClient::getTopic("$share/queue/" + topicName);
    for (size_t index = 0; index < clientCount; ++index)
    {
        const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
        connectMessageParameters->m_cleanSession = true;
        connectMessageParameters->setClientId(format("{}_{}", subscriberClientId, index));
        const auto clientSession = ClientSession::factory(server().get(), connectMessageParameters);
        subscriptionManager->subscribe(sharedTopic, clientSession, Qos::Qos0, 0, SubscriptionOptions());
        clientSessions.push_back(clientSession);
    }

    // Create a test message
    const auto* topic = client::MqttClient::getTopic(topicName);
    const auto  message = make_shared<mqtt::PublishMessage>(topic, string_view("Payload"));

    // Deliver two messages per client session
    constexpr auto messagesPerClient = 2;
    for (size_t index = 0; index < messagesPerClient * clientSessions.size(); ++index)
    {
        subscriptionManager->publishMessage(message);
    }

    // Expect 2 messages delivered to each of the client sessions (Round-Robin distribution)
    EXPECT_EQ(2U, clientSessions[0]->getPublishScheduledCount());
    EXPECT_EQ(2U, clientSessions[1]->getPublishScheduledCount());
    EXPECT_EQ(2U, clientSessions[2]->getPublishScheduledCount());

    for (const auto& clientSession: clientSessions)
    {
        subscriptionManager->unsubscribe(sharedTopic, clientSession.get());
        clientSession->clearSession();
    }
}

TEST_F(XMQ_ServerTests, SharedSubscription_SubscribeToWildcardInternally)
{
    const auto storage = server()->getStorage();
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto publishTopic = topicName + "/1";
    const auto subscribeTopic = "$share/group1/" + topicName + "/#";

    // Create several client sessions and subscribe them to the same shared queue
    constexpr auto         clientCount {3};
    vector<SClientSession> clientSessions;
    const auto*            sharedTopic = client::MqttClient::getTopic(subscribeTopic);
    for (size_t index = 0; index < clientCount; ++index)
    {
        const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
        connectMessageParameters->m_cleanSession = true;
        connectMessageParameters->setClientId(format("{}_{}", subscriberClientId, index));
        const auto clientSession = ClientSession::factory(server().get(), connectMessageParameters);
        subscriptionManager->subscribe(sharedTopic, clientSession, Qos::Qos0, 0, SubscriptionOptions());
        clientSessions.push_back(clientSession);
    }

    // Create a test message
    const auto* topic = client::MqttClient::getTopic(publishTopic);
    const auto  message = make_shared<mqtt::PublishMessage>(topic, string_view("Payload"));

    // Deliver two messages per client session
    constexpr auto messagesPerClient = 2;
    for (size_t index = 0; index < messagesPerClient * clientSessions.size(); ++index)
    {
        subscriptionManager->publishMessage(message);
    }

    // Expect 2 messages delivered to each of the client sessions (Round-Robin distribution)
    EXPECT_EQ(2U, clientSessions[0]->getPublishScheduledCount());
    EXPECT_EQ(2U, clientSessions[1]->getPublishScheduledCount());
    EXPECT_EQ(2U, clientSessions[2]->getPublishScheduledCount());

    for (const auto& clientSession: clientSessions)
    {
        subscriptionManager->unsubscribe(sharedTopic, clientSession.get());
        clientSession->clearSession();
    }
}

TEST_F(XMQ_ServerTests, SharedSubscription_Unsubscribe)
{
    const auto storage = server()->getStorage();
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto subscriptionManager = server()->getSubscriptionManager();

    // Create several client sessions and subscribe them to the same shared queue
    constexpr auto         clientCount {3};
    vector<SClientSession> clientSessions;
    const auto             sharedTopic = client::MqttClient::getTopic("$share/queue/" + topicName);
    for (size_t index = 0; index < clientCount; ++index)
    {
        const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
        connectMessageParameters->m_cleanSession = true;
        connectMessageParameters->setClientId(format("{}_{}", subscriberClientId, index));
        const auto clientSession = ClientSession::factory(server().get(), connectMessageParameters);
        subscriptionManager->subscribe(sharedTopic, clientSession, Qos::Qos0, 0, SubscriptionOptions());
        clientSessions.push_back(clientSession);
    }

    // Create a test message
    const auto message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic(topicName), string_view("Payload"));

    // Deliver three messages to client sessions
    constexpr auto messageCount = 3;
    for (size_t index = 0; index < messageCount; ++index)
    {
        subscriptionManager->publishMessage(message);
    }

    // Expect 1 message delivered to each client session
    size_t deliveredCount = 0;
    for (const auto& clientSession: clientSessions)
    {
        EXPECT_EQ(1U, clientSession->getPublishScheduledCount());
        deliveredCount += clientSession->getPublishScheduledCount();
    }
    EXPECT_EQ(3U, deliveredCount);

    // Unsubscribe a session and send two more messages
    subscriptionManager->unsubscribe(sharedTopic, clientSessions[1].get());

    for (size_t index = 0; index < messageCount - 1; ++index)
    {
        subscriptionManager->publishMessage(message);
    }

    deliveredCount = 0;
    for (const auto& clientSession: clientSessions)
    {
        deliveredCount += clientSession->getPublishScheduledCount();
    }
    EXPECT_EQ(5U, deliveredCount);

    // Clean up all
    for (const auto& clientSession: clientSessions)
    {
        subscriptionManager->unsubscribe(sharedTopic, clientSession.get());
        clientSession->clearSession();
    }
}
