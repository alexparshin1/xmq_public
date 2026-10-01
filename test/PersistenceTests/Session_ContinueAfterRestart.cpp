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
#include "storage/RedisStorage.h"
#include "test/PersistenceTests/PersistenceTests.h"
#include "test/TestServers.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

// Time allowed for a subscribe/publish to reach the server and be persisted before the
// client disconnects. Only used where there is no acknowledgement to wait on.
constexpr auto settleTime = 100ms;

const auto storageUri = TestServers::dbConnectString(TestServers::StorageType::PostgreSQL);

shared_ptr<client::MqttClient> connectClient(const string&                 clientId,
                                             const bool                    cleanSession = false,
                                             const PublishMessageCallback& messageCallback = nullptr)
{
    auto client = make_shared<client::MqttClient>(XMQ_PersistenceTests::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = cleanSession;

    if (messageCallback)
    {
        client->onMessage(messageCallback);
    }

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", XMQ_PersistenceTests::TestTcpPortNumber),
                  ConnectCredentials(clientId, "user", "secret"),
                  connectParameters, ProtocolVersion::MqttV31));

    return client;
}

// Subscribe, then disconnect - leaving a session the server must remember.
void createOfflineSession(const string& clientId, const list<string>& topicNames, const bool cleanSession = false)
{
    const auto subscriber = connectClient(clientId, cleanSession);
    for (const auto& topicName: topicNames)
    {
        subscriber->subscribe(topicName);
    }
    this_thread::sleep_for(settleTime);
    subscriber->disconnect();
}

void publishOneMessagePerTopic(const string& clientId, const list<string>& topicNames, const Qos qos = Qos::Qos1)
{
    const auto publisher = connectClient(clientId);
    for (const auto& topicName: topicNames)
    {
        publisher->publish(topicName, "message in " + topicName, qos);
    }
    this_thread::sleep_for(settleTime);
    publisher->disconnect();
}

// Reconnect and collect deliveries, returning how many arrived within the timeout.
size_t receiveQueuedMessages(const string& clientId, const size_t expectedCount)
{
    atomic_size_t messageCount = 0;
    Semaphore     allReceived;

    const auto subscriber = connectClient(clientId, false,
                                          [&messageCount, &allReceived, expectedCount](const SPublishMessage&)
                                          {
                                              if (++messageCount == expectedCount)
                                              {
                                                  allReceived.post();
                                              }
                                          });

    constexpr auto deliveryTimeout = 1000ms;
    EXPECT_TRUE(allReceived.wait_for(deliveryTimeout));

    subscriber->disconnect();
    return messageCount;
}

} // namespace

void XMQ_PersistenceTests::expectSessionInRedis(const SRedisConnect& redis, const string& clientId, const size_t expectedMessageCount)
{
    EXPECT_FALSE(redis->getValue("session_" + clientId).isNull())
        << "Session " << clientId << " is not in Redis";

    const auto nodeSessions = redis->getSetMembers(format("node_{}_sessions", server()->getNodeName()));
    EXPECT_NE(ranges::find(nodeSessions, clientId), nodeSessions.end())
        << "Session " << clientId << " is not in its node's session set - it will not be restored";

    EXPECT_EQ(expectedMessageCount, redis->getHashValues("session_" + clientId + "_messages").size())
        << "Unexpected number of queued messages in Redis for " << clientId;
}

void XMQ_PersistenceTests::expectRestoredSession(const string& clientId, const list<string>& topicNames, const size_t expectedMessageCount)
{
    const auto session = server()->getClientSessionManager()->find(clientId);
    ASSERT_TRUE(session) << "Session " << clientId << " was not restored";

    const auto& subscribedTo = session->getSubscribedTo();
    EXPECT_EQ(topicNames.size(), subscribedTo.size());
    for (const auto& topicName: topicNames)
    {
        EXPECT_TRUE(subscribedTo.contains(topicName))
            << "Subscription to " << topicName << " was not restored";
    }

    EXPECT_EQ(expectedMessageCount, session->enqueuedMessages().size())
        << "Unexpected number of queued messages restored for " << clientId;
}

SServer XMQ_PersistenceTests::restartServer()
{
    stopServers();
    return createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, false);
}

// A persistent session that is offline across a restart keeps its subscriptions and its
// undelivered messages, and gets them on reconnect.
TEST_F(XMQ_PersistenceTests, Session_ContinueAfterServerRestart)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const list topicNames{topicName + "_1", topicName + "_2"};
    const auto messageCount = topicNames.size();

    createOfflineSession(subscriberClientId, topicNames);
    publishOneMessagePerTopic(publisherClientId, topicNames);

    expectSessionInRedis(server->getRedisStorage()->getRedis(), subscriberClientId, messageCount);
    expectRestoredSession(subscriberClientId, topicNames, messageCount);

    server = restartServer();

    // Before any client reconnects - so this measures what startup restored, not what a
    // CONNECT loaded.
    expectSessionInRedis(server->getRedisStorage()->getRedis(), subscriberClientId, messageCount);
    expectRestoredSession(subscriberClientId, topicNames, messageCount);

    const auto restoredCount = server->getClientSessionManager()->clientCount();
    EXPECT_EQ(restoredCount, server->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsDisconnected));
    EXPECT_EQ(restoredCount, server->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsTotal));

    EXPECT_EQ(messageCount, receiveQueuedMessages(subscriberClientId, messageCount));

    stopServers();
}

// A clean session is deliberately not remembered across a restart.
TEST_F(XMQ_PersistenceTests, Session_CleanSessionNotRestoredAfterServerRestart)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    createOfflineSession(subscriberClientId, {topicName}, true);

    server = restartServer();

    EXPECT_FALSE(server->getClientSessionManager()->find(subscriberClientId))
        << "A clean session must not survive a restart";
    EXPECT_TRUE(server->getRedisStorage()->getRedis()->getValue("session_" + subscriberClientId).isNull())
        << "A clean session must not be left behind in Redis";

    stopServers();
}

// A restored subscription must be live, not just present: a message published after the
// restart, while the session is still offline, has to be queued for it.
TEST_F(XMQ_PersistenceTests, Session_RestoredSubscriptionReceivesNewMessages)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const list topicNames{topicName};

    createOfflineSession(subscriberClientId, topicNames);

    server = restartServer();

    // Nothing queued yet - the session was offline with no traffic.
    expectRestoredSession(subscriberClientId, topicNames, 0);

    publishOneMessagePerTopic(publisherClientId, topicNames);

    expectRestoredSession(subscriberClientId, topicNames, 1);
    EXPECT_EQ(1U, receiveQueuedMessages(subscriberClientId, 1));

    stopServers();
}

// QoS2 publishes must survive a restart the same way QoS1 does.
TEST_F(XMQ_PersistenceTests, Session_Qos2MessagesRestoredAfterServerRestart)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const list topicNames{topicName + "_1", topicName + "_2"};
    const auto messageCount = topicNames.size();

    createOfflineSession(subscriberClientId, topicNames);
    publishOneMessagePerTopic(publisherClientId, topicNames, Qos::Qos2);

    expectSessionInRedis(server->getRedisStorage()->getRedis(), subscriberClientId, messageCount);

    server = restartServer();

    expectRestoredSession(subscriberClientId, topicNames, messageCount);
    EXPECT_EQ(messageCount, receiveQueuedMessages(subscriberClientId, messageCount));

    stopServers();
}
