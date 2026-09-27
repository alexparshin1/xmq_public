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
#include "base/MessageProperties.h"
#include "storage/RedisStorage.h"
#include "test/PersistenceTests/PersistenceTests.h"
#include "test/TestServers.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

// Time allowed for a subscribe or publish to reach the server and be persisted before the
// client disconnects. Only used where there is no acknowledgement to wait on.
constexpr auto settleTime = 100ms;

// Message expiry is carried in whole seconds, so the shortest interval that can be expressed
// is one. Two is used to leave room for the publish to be persisted well inside the lifetime,
// which is what makes the "still there" assertion meaningful.
constexpr uint32_t messageExpirySeconds = 2;

// Waited after the interval has passed. Generous rather than tight: the assertion that matters
// is that the message is eventually gone, and a marginal wait would make the test flaky on a
// loaded build host rather than catch anything extra.
constexpr auto expiryOverrun = 3s;

// The session must outlive the client for the message to be queued at all. MQTT 5 ends a
// session at disconnect unless the CONNECT says otherwise - clean start being false is not
// enough on its own, unlike in 3.1.1.
constexpr uint32_t sessionExpirySeconds = 3600;

shared_ptr<client::MqttClient> connectMqtt5Client(const string& clientId)
{
    auto client = make_shared<client::MqttClient>(XMQ_PersistenceTests::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = false;

    const auto connectProperties = make_shared<MessageProperties>();
    connectProperties->setProperty(Property::SessionExpiryInterval, sessionExpirySeconds);

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", XMQ_PersistenceTests::TestTcpPortNumber),
                              ConnectCredentials(clientId, "user", "secret"),
                              connectParameters, ProtocolVersion::MqttV5, connectProperties));

    return client;
}

// Subscribe at QoS 1, then disconnect - leaving a persistent session the server must queue for.
void createOfflineMqtt5Session(const string& clientId, const string& topicName)
{
    const auto subscriber = connectMqtt5Client(clientId);
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));
    this_thread::sleep_for(settleTime);
    subscriber->disconnect();
}

void publishWithExpiry(const string& clientId, const string& topicName, const uint32_t expirySeconds)
{
    const auto publisher = connectMqtt5Client(clientId);

    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::MessageExpiryInterval, expirySeconds);

    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("This message expires"),
                       Qos::Qos1, properties);

    this_thread::sleep_for(settleTime);
    publisher->disconnect();
}

size_t queuedMessagesInRedis(const SRedisConnect& redis, const string& clientId)
{
    return redis->getHashValues("session_" + clientId + "_messages").size();
}

} // namespace

// A message published with an expiry interval, to a session that is offline, must not outlive
// that interval in storage. Nothing will ever deliver it once it has expired, so a record left
// behind is queued for a delivery that cannot happen - it is restored at every restart and
// counted against the session's queue limit for as long as the session exists.
TEST_F(XMQ_PersistenceTests, Message_ExpiredMessageRemovedFromStorage)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    createOfflineMqtt5Session(subscriberClientId, topicName);
    publishWithExpiry(publisherClientId, topicName, messageExpirySeconds);

    const auto redis = server->getRedisStorage()->getRedis();

    // Inside its lifetime the message is queued for the offline session, in memory and in Redis.
    expectSessionInRedis(redis, subscriberClientId, 1);
    expectRestoredSession(subscriberClientId, {topicName}, 1);

    this_thread::sleep_for(chrono::seconds(messageExpirySeconds) + expiryOverrun);

    // Past it, the message is undeliverable and must be gone - while the session itself, whose
    // own expiry is an hour away, stays.
    EXPECT_FALSE(redis->getValue("session_" + subscriberClientId).isNull())
        << "The session expires in " << sessionExpirySeconds << "s and must outlive its message";
    EXPECT_EQ(0U, queuedMessagesInRedis(redis, subscriberClientId))
        << "An expired message is still queued in Redis for " << subscriberClientId;
    expectRestoredSession(subscriberClientId, {topicName}, 0);

    stopServers();
}
