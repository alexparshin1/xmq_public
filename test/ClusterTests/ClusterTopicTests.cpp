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

#include "ClusterTests/ClusterTests.h"
#include "ServerTests/ServerTests.h"
#include "client/MqttClient.h"
#include "common/ConnectCredentials.h"
#include "common/SubscribeAckMessage.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

void publishTopicRestrictions(const Host&   server, const ConnectCredentials&                credentials,
                              const uint8_t clusterTopicResult, const shared_ptr<LogEngine>& logEngine)
{
    const auto publisher = make_shared<client::MqttClient>(logEngine);

    Semaphore publishAckReceived;
    uint8_t   publishResult{0};
    publisher->onAck([&publishAckReceived, &publishResult](const SMessage& message)
    {
        if (message->is(Message::Type::PublishAck))
        {
            if (const auto publishAck = dynamic_pointer_cast<AckMessage>(message))
            {
                publishResult = static_cast<uint8_t>(publishAck->getReasonCode());
                publishAckReceived.post();
            }
        }
    });

    const auto sslKeys = credentials.getUsername() == "cluster" ? make_shared<SSLKeys>() : nullptr;
    const auto rc = publisher->connect(server, credentials, {.m_cleanSession = true}, ProtocolVersion::MqttV5, {}, sslKeys);
    ASSERT_EQ(ReasonCode::Success, rc);

    // Any user can publish to regular topics
    publisher->publish(client::MqttClient::getTopic("topic/state/connected"), Buffer("Hello, World!"), Qos::Qos1);
    ASSERT_TRUE(publishAckReceived.wait_for(100ms));
    EXPECT_EQ(0, publishResult);

    // No user can publish to system topics
    publisher->publish(client::MqttClient::getTopic("$SYS/version"), Buffer("1.0.0"), Qos::Qos1);
    ASSERT_TRUE(publishAckReceived.wait_for(100ms));
    EXPECT_EQ(static_cast<uint8_t>(ReasonCode::NotAuthorized), publishResult);

    // Cluster user can publish to cluster topics
    publisher->publish(client::MqttClient::getTopic("$CLUSTER/C"), Buffer("anode"), Qos::Qos1);
    ASSERT_TRUE(publishAckReceived.wait_for(100ms));
    EXPECT_EQ(clusterTopicResult, publishResult);
}

void subscribeTopicRestrictions(const ProtocolVersion  protocolVersion, const Host&                     server, const ConnectCredentials& credentials,
                                uint8_t, const uint8_t clusterTopicResult, const shared_ptr<LogEngine>& logEngine)
{
    const auto subscriber = make_shared<client::MqttClient>(logEngine);

    Semaphore subscribeAckReceived;
    uint8_t   subscriptionResult{0};
    subscriber->onAck([&subscribeAckReceived, &subscriptionResult](const SMessage& message)
    {
        if (const auto subscribeAck = dynamic_pointer_cast<SubscribeAckMessage>(message))
        {
            ASSERT_EQ(1U, subscribeAck->subscriptionResults().size());
            subscriptionResult = subscribeAck->subscriptionResults()[0];
            subscribeAckReceived.post();
        }
    });

    const auto sslKeys = credentials.getUsername() == "cluster" ? make_shared<SSLKeys>() : nullptr;
    const auto rc = subscriber->connect(server, credentials, {.m_cleanSession = true}, protocolVersion, {}, sslKeys);
    ASSERT_EQ(ReasonCode::Success, rc);

    // Any user can subscribe to regular topics
    subscriber->subscribe(Destination(client::MqttClient::getTopic("topic/connected")));
    ASSERT_TRUE(subscribeAckReceived.wait_for(100ms));
    EXPECT_EQ(1, subscriptionResult);

    // Any user can subscribe to system topics
    subscriber->subscribe(Destination(client::MqttClient::getTopic("$SYS/version")));
    ASSERT_TRUE(subscribeAckReceived.wait_for(100ms));
    EXPECT_EQ(1, subscriptionResult);

    // Only cluster user can subscribe to cluster topics
    subscriber->subscribe(Destination(client::MqttClient::getTopic("$CLUSTER/state/connected")));
    ASSERT_TRUE(subscribeAckReceived.wait_for(100ms));
    EXPECT_EQ(clusterTopicResult, subscriptionResult);
}

void subscribeWildcardRestrictions(const Host&   server, const XMQ_ServerTests::TestNames& testNames,
                                   const string& wildcard, const shared_ptr<LogEngine>&    logEngine)
{
    const auto publisher = make_shared<client::MqttClient>(logEngine);
    const auto subscriber = make_shared<client::MqttClient>(logEngine);

    Semaphore messageReceived;
    subscriber->onMessage([&messageReceived](const SPublishMessage&)
    {
        messageReceived.post();
    });

    ConnectCredentials credentials{testNames.m_subscriberClientId, "cluster", "cluster"};
    auto               rc = subscriber->connect(server, credentials, {.m_cleanSession = true}, ProtocolVersion::MqttV5, {}, make_shared<SSLKeys>());
    ASSERT_EQ(ReasonCode::Success, rc);

    ConnectCredentials credentials2(testNames.m_publisherClientId, "cluster", "cluster");
    rc = publisher->connect(server, credentials2, {.m_cleanSession = true}, ProtocolVersion::MqttV5, {}, make_shared<SSLKeys>());
    ASSERT_EQ(ReasonCode::Success, rc);

    // Subscribe to a wildcard
    subscriber->subscribe(Destination(client::MqttClient::getTopic(wildcard)));
    this_thread::sleep_for(10ms);

    publisher->publish(client::MqttClient::getTopic("$CLUSTER/request/node_detached"), Buffer("anode"), Qos::Qos1);

    if (messageReceived.wait_for(100ms))
    {
        FAIL() << "Not expected message received for '" << wildcard << "'";
    }
}

} // namespace

/**
 * Verify that the client with the username 'user' can publish to the regular topic
 * but can't publish to "$CLUSTER/" topics.
 */
TEST_F(XMQ_ClusterTests, publishUserAccess)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    createNode("primary", 1880, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{subscriberClientId, "user", "secret"};

    // Not testing for MQTT3* as PubAck only has reason code in MQTT5.
    publishTopicRestrictions(m_primaryServerHost, credentials, static_cast<uint8_t>(ReasonCode::NotAuthorized), logEngine());
}

/**
 * Verify that the client with the username 'cluster' can publish to the regular topic and
 * to "$CLUSTER/" topics.
 */
TEST_F(XMQ_ClusterTests, publishClusterUserAccess)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    createNode("primary", 1880, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{subscriberClientId, "cluster", "cluster"};

    // Not testing for MQTT3* as PubAck only has reason code in MQTT5.
    publishTopicRestrictions(Host("localhost", 8880), credentials, 0, logEngine());
}

/**
 * Verify that the client with the username 'user' can subscribe to the regular topic
 * but can't subscribe to "$CLUSTER/" topics.
 */
TEST_F(XMQ_ClusterTests, subscribeUserAccess)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    createNode("primary", 1880, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{subscriberClientId, "user", "secret"};

    subscribeTopicRestrictions(ProtocolVersion::MqttV31, m_primaryServerHost, credentials,
                               1, static_cast<uint8_t>(ReasonCode::UnspecifiedError), logEngine());

    subscribeTopicRestrictions(ProtocolVersion::MqttV5, m_primaryServerHost, credentials,
                               1, static_cast<uint8_t>(ReasonCode::NotAuthorized), logEngine());
}

/**
 * Verify that the client with the username 'cluster' can subscribe to the regular topic
 * but can subscribe to "$CLUSTER/" topics.
 */
TEST_F(XMQ_ClusterTests, subscribeClusterUserAccess)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    createNode("primary", 1880, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials{subscriberClientId, "cluster", "cluster"};

    subscribeTopicRestrictions(ProtocolVersion::MqttV31, Host("localhost", 8880), credentials,
                               static_cast<uint8_t>(ReasonCode::UnspecifiedError), 1, logEngine());
    subscribeTopicRestrictions(ProtocolVersion::MqttV5, Host("localhost", 8880), credentials,
                               static_cast<uint8_t>(ReasonCode::NotAuthorized), 1, logEngine());
}

TEST_F(XMQ_ClusterTests, subscribeWildcardRestrictions)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    createNode("primary", 1880, true);

    const auto testNames = makeTestNames();

    subscribeWildcardRestrictions(Host("localhost", 8880), testNames, "#", logEngine());
}