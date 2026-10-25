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
#include "test/ServerTests/ExternalClient/ExternalClient.h"
#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"
#include "test/SubscribeAndWait.h"
#include "test/TestServers.h"
#include <sptk5/net/RedisConnect.h>

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTests::testSessionReconnect(bool cleanSession)
{
    constexpr auto qos = Qos::Qos1;
    const auto     protocolVersion = GetParam();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // Subscribe a client to the topic and exit
    shared_ptr<client::MqttClient> subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, cleanSession, false, protocolVersion);
    ASSERT_TRUE(subscriber->isConnected());
    ASSERT_TRUE(test::subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(qos))));
    subscriber->disconnect();

    // Publish one message while the subscriber is not connected
    const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion);
    ASSERT_TRUE(publisher->isConnected());
    const auto published = make_shared<Semaphore>();
    publisher->onAck([published](const SMessage& message)
                     {
                         if (message->is(Message::Type::PublishAck))
                         {
                             published->post();
                         }
                     });
    publisher->publish(topicName, "Test Data", qos);
    ASSERT_TRUE(published->wait_for(2s));

    // Install the receiver before reconnecting: queued messages may arrive with CONNACK.
    // For a persistent session, don't re-subscribe to the same topic as we expect subscriptions to be preserved.
    subscriber = make_shared<client::MqttClient>(logEngine());
    const auto messageCount = make_shared<atomic_size_t>(0);
    const auto received = make_shared<Semaphore>();
    subscriber->onMessage([messageCount, received](const SPublishMessage&)
                          {
                              ++*messageCount;
                              received->post();
                          });
    ASSERT_EQ(ReasonCode::Success,
              subscriber->connect(Host("localhost", TestTcpPortNumber), {subscriberClientId, "user", "secret"},
                                  {.m_cleanSession = cleanSession}, protocolVersion));
    if (cleanSession)
    {
        ASSERT_TRUE(test::subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(qos))));
    }

    if (cleanSession)
    {
        EXPECT_FALSE(received->wait_for(100ms));
    }
    else
    {
        EXPECT_TRUE(received->wait_for(2s));
    }
    subscriber->onMessage({});
    EXPECT_EQ(cleanSession ? 0U : 1U, messageCount->load());
}

TEST_P(XMQ_ServerTests, Session_Clean)
{
    testSessionReconnect(true);
}

TEST_P(XMQ_ServerTests, Session_Continue)
{
    testSessionReconnect(false);
}

TEST_F(XMQ_ServerTests, Session_Clean_RemovesRedisState)
{
    const auto clientId = makeTestNames().m_subscriberClientId;
    const auto host = Host("localhost", TestTcpPortNumber);
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    const auto waitFor = [](const function<bool()>& predicate, const chrono::milliseconds timeout)
    {
        const auto deadline = chrono::steady_clock::now() + timeout;
        do
        {
            if (predicate())
            {
                return true;
            }
            this_thread::sleep_for(10ms);
        } while (chrono::steady_clock::now() < deadline);
        return predicate();
    };

    const auto sessionKey = "session_" + clientId;
    const auto messagesKey = sessionKey + "_messages";
    const auto qos2Key = sessionKey + "_qos2";
    const auto ownerKey = sessionKey + "_owner";
    const auto nodeSessionsKey = "node_primary_sessions";
    const auto redisKeyExists = [&redis](const string& key)
    {
        return !redis.scan(key, 10).empty();
    };
    const auto stateExists = [&]
    {
        return redisKeyExists(sessionKey) || redis.isSetMember(nodeSessionsKey, clientId) ||
               redisKeyExists(messagesKey) || redisKeyExists(qos2Key) || redisKeyExists(ownerKey);
    };

    auto persistentClient = make_shared<client::MqttClient>();
    ASSERT_EQ(ReasonCode::Success,
              persistentClient->connect(host, {clientId, "user", "secret"}, {.m_cleanSession = false},
                                        ProtocolVersion::MqttV5));
    ASSERT_TRUE(persistentClient->isConnected());
    ASSERT_TRUE(waitFor([&]
                        {
                            return redis.isSetMember(nodeSessionsKey, clientId) &&
                                   !redis.getValue(sessionKey).isNull();
                        }, 5s)) << "persistent session was not stored in Redis";
    persistentClient->disconnect();

    auto cleanClient = make_shared<client::MqttClient>();
    ASSERT_EQ(ReasonCode::Success,
              cleanClient->connect(host, {clientId, "user", "secret"}, {.m_cleanSession = true},
                                   ProtocolVersion::MqttV5));
    ASSERT_TRUE(cleanClient->isConnected());
    cleanClient->disconnect();

    const auto cleared = waitFor([&]
                                 {
                                     return !stateExists();
                                 }, 5s);
    EXPECT_TRUE(cleared) << "clean session left Redis state for " << clientId
                         << ": session=" << !redis.getValue(sessionKey).isNull()
                         << ", node membership=" << redis.isSetMember(nodeSessionsKey, clientId)
                         << ", qos2=" << redisKeyExists(qos2Key)
                         << ", owner=" << redisKeyExists(ownerKey)
                         << ", matching keys=" << redis.scan("*" + clientId + "*", 100).size();
}

TEST_F(XMQ_ServerTests, Session_Expiration)
{
    constexpr auto protocolVersion = ProtocolVersion::MqttV5;

    debugLog(false);

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    Semaphore subscribed;
    client->onAck([&subscribed](const SMessage& message)
                  {
                      if (message->is(Message::Type::SubscribeAck))
                      {
                          subscribed.post();
                      }
                  });

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");
    const auto               properties = make_shared<MessageProperties>();
    properties->setProperty(Property::SessionExpiryInterval, 1);
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestTcpPortNumber), credentials,
                              {
                                  .m_keepAliveInterval = 60s,
                                  .m_cleanSession = false,
                              },
                              protocolVersion, properties));

    this_thread::sleep_for(10ms);

    const Destinations destinations({Destination(client::MqttClient::getTopic(topicName))});
    client->subscribe(destinations, properties);

    this_thread::sleep_for(10ms);
    client->disconnect();

    // Check if the session exists:
    auto clientSession = server()->getClientSession(subscriberClientId);
    ASSERT_TRUE(clientSession);

    this_thread::sleep_for(1100ms);

    clientSession = server()->getClientSession(subscriberClientId);
    if (clientSession)
    {
        FAIL() << "Session " << subscriberClientId << " should be expired";
    }
}
