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

#include "test/PersistenceTests/PersistenceTests.h"

#include "storage/RedisStorage.h"
#include "test/ServerTests/ServerTestsLink.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTestsLink::linkPersitenceTests()
{
    // Force linking this module
}

void XMQ_PersistenceTests::SetUp()
{
    ServerTests_Suite::SetUp();

    stopServers();
    createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);
}

SClientSession XMQ_PersistenceTests::createClientSession(const std::string& clientId, const vector<string>& topicNames, const vector<string>& messagePayloadsPerTopic)
{
    // Create persistent session
    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    connectMessageParameters->setClientId(clientId);
    connectMessageParameters->m_cleanSession = false;
    auto clientSession = ClientSession::factory(server().get(), connectMessageParameters);

    const auto subscriptionManager = server()->getSubscriptionManager();
    for (const auto& topicName: topicNames)
    {
        const auto* topic = server()->getTopic(topicName);

        // Create subscription:
        subscriptionManager->subscribe(topic, clientSession, Qos::Qos1, 1234, SubscriptionOptions(Qos::Qos2));

        // Send messages to just created subscription:
        for (const auto& messagePayload: messagePayloadsPerTopic)
        {
            const auto message = make_shared<mqtt::PublishMessage>(topic, messagePayload);
            message->setQos(Qos::Qos1);
            subscriptionManager->publishMessage(message);
        }
    }

    return clientSession;
}

bool XMQ_PersistenceTests::isClientSessionInRedis(const SClientSession& clientSession)
{
    const auto redis = server()->getRedisStorage()->getRedis();
    const auto sessionKey = "session_" + clientSession->getClientId();
    const auto sessionInfo = redis->getValue(sessionKey);
    return !sessionInfo.isNull();
}

bool XMQ_PersistenceTests::areClientSessionMessagesInRedis(const SClientSession& clientSession, const size_t expectedMessageCount)
{
    const auto redis = server()->getRedisStorage()->getRedis();
    const auto sessionMessagesKey = "session_" + clientSession->getClientId() + "_messages";
    if (const auto sessionMessages = redis->getHashValues(sessionMessagesKey);
        sessionMessages.size() != expectedMessageCount)
    {
        CERR("Expected " << expectedMessageCount << " client session messages in Redis, but got " << sessionMessages.size());
        return false;
    }

    return true;
}

bool XMQ_PersistenceTests::isClientSessionInRedis(const SClientSession& clientSession, size_t expectedMessageCount)
{
    if (!isClientSessionInRedis(clientSession))
    {
        return false;
    }

    if (!areClientSessionMessagesInRedis(clientSession, expectedMessageCount))
    {
        return false;
    }

    return true;
}

TEST_F(XMQ_PersistenceTests, createAndCheckClientSession)
{
    // Create persistent session
    const auto clientId = format("test_client_{}", DateTime::Now().timePoint().time_since_epoch().count() % 1000);

    SClientSession clientSession;
    EXPECT_NO_THROW(clientSession = createClientSession(clientId));

    this_thread::sleep_for(10ms);

    // Find session:
    EXPECT_TRUE(isClientSessionInRedis(clientSession, 0));

    // Delete the persistent session object - the database record should be removed.
    clientSession->unpersist();

    this_thread::sleep_for(10ms);

    EXPECT_FALSE(isClientSessionInRedis(clientSession, 0));

    stopServers();
}

TEST_F(XMQ_PersistenceTests, createAndRestoreClientSession)
{
    const auto logger = debugLog(true);

    // Create persistent session
    const auto clientId = format("test_client_{}", DateTime::Now().timePoint().time_since_epoch().count() % 1000);

    SClientSession clientSession;
    vector<string> topicNames{"topic/1", "topic/2"};
    EXPECT_NO_THROW(clientSession = createClientSession(clientId, topicNames, {"Test message 1", "Test message 2"}));

    this_thread::sleep_for(10ms);

    EXPECT_TRUE(isClientSessionInRedis(clientSession, 4));

    logger->info("Remove session");
    server()->getClientSessionManager()->remove(clientSession);

    EXPECT_TRUE(isClientSessionInRedis(clientSession, 4));

    // Reload session:
    EXPECT_NO_THROW(clientSession = createClientSession(clientId));

    auto subscribedTopics = clientSession->getSubscribedTo();
    ASSERT_EQ(subscribedTopics.size(), 2U);

    // Check session subscriptions:
    EXPECT_EQ(subscribedTopics[topicNames[0]].subscription->fullName(), topicNames[0]);
    EXPECT_EQ(subscribedTopics[topicNames[1]].subscription->fullName(), topicNames[1]);

    // Remove session from the database:
    clientSession->unpersist();
    this_thread::sleep_for(10ms);
    EXPECT_FALSE(isClientSessionInRedis(clientSession));
    EXPECT_TRUE(areClientSessionMessagesInRedis(clientSession, 0));

    stopServers();
}