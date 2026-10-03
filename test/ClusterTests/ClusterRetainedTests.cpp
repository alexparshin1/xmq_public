/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Confirm that a retained publication and its later deletion reach another cluster node even
 * when that node has no subscriber for the topic.
 *
 * Setup: Join two nodes. Publish a retained value on the primary before subscribing on the
 * secondary; disconnect that subscriber before publishing the empty retained value.
 *
 * Verification: Poll the secondary's retained store for the value, subscribe there and check
 * that the retained payload is delivered, then poll the store until the clearing publication
 * removes the value while no client on the secondary is subscribed.
 */
TEST_F(XMQ_ClusterTests, retainedMessageReplicatesWithoutSubscribers)
{
    auto [primary, secondary] = makeClusterOfTwoNodes();
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    auto publisher = make_shared<client::MqttClient>(logEngine());
    ASSERT_EQ(ReasonCode::Success,
              publisher->connect(m_primaryServerHost, ConnectCredentials(publisherId, "user", "secret"),
                                 {.m_cleanSession = true}, ProtocolVersion::MqttV5));

    publisher->publish(client::MqttClient::getTopic(topic), Buffer("retained value"), Qos::Qos1, {}, true);

    const auto replicated = [&]
    {
        bool found = false;
        secondary->getSubscriptionManager()->retainedMessages().forEachMatching(
            topic, [&found](const string&, const RetainedMessages::Record& record)
            {
                found = record.m_payload == "retained value";
            });
        return found;
    };
    for (auto i = 0; i < 100 && !replicated(); ++i) this_thread::sleep_for(20ms);
    ASSERT_TRUE(replicated()) << "A node without subscribers did not store the retained publication";

    Semaphore received;
    string payload;
    auto subscriber = make_shared<client::MqttClient>(logEngine());
    subscriber->onMessage([&payload, &received](const SPublishMessage& message)
                          {
                              payload.assign(reinterpret_cast<const char*>(message->payloadData()), message->payloadSize());
                              received.post();
                          });
    ASSERT_EQ(ReasonCode::Success,
              subscriber->connect(m_secondaryServerHost, ConnectCredentials(subscriberId, "user", "secret"),
                                  {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    subscriber->subscribe(topic);
    ASSERT_TRUE(received.wait_for(1s));
    EXPECT_EQ("retained value", payload);

    subscriber->disconnect();
    publisher->publish(client::MqttClient::getTopic(topic), Buffer(""), Qos::Qos1, {}, true);

    const auto cleared = [&]
    {
        bool found = false;
        secondary->getSubscriptionManager()->retainedMessages().forEachMatching(
            topic, [&found](const string&, const RetainedMessages::Record&) { found = true; });
        return !found;
    };
    for (auto i = 0; i < 100 && !cleared(); ++i) this_thread::sleep_for(20ms);
    EXPECT_TRUE(cleared()) << "A node did not clear its copy of the retained message";
}

/**
 * Confirm that a node joining an existing cluster receives retained state published earlier,
 * without requiring any client subscription on the joining node.
 *
 * Setup: Join two nodes, publish a retained value on the primary, and wait until the primary
 * has stored it. Only then start a third node and attach it to the cluster.
 *
 * Verification: Poll the third node's retained store for the topic and compare its payload
 * with the value published before that node joined.
 */
TEST_F(XMQ_ClusterTests, joiningNodeReceivesRetainedMessagesWithoutSubscribers)
{
    auto [primary, secondary] = makeClusterOfTwoNodes();
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    auto publisher = make_shared<client::MqttClient>(logEngine());
    ASSERT_EQ(ReasonCode::Success,
              publisher->connect(m_primaryServerHost, ConnectCredentials(publisherId, "user", "secret"),
                                 {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    publisher->publish(client::MqttClient::getTopic(topic), Buffer("before joining"), Qos::Qos1, {}, true);

    bool storedOnPrimary = false;
    for (auto i = 0; i < 100 && !storedOnPrimary; ++i)
    {
        primary->getSubscriptionManager()->retainedMessages().forEachMatching(
            topic, [&storedOnPrimary](const string&, const RetainedMessages::Record& record)
            {
                storedOnPrimary = record.m_payload == "before joining";
            });
        if (!storedOnPrimary) this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(storedOnPrimary);

    auto third = createNode("third", 1881, false);
    third->attachToCluster(primary->getCluster()->getNodeHost());

    bool storedOnThird = false;
    for (auto i = 0; i < 100 && !storedOnThird; ++i)
    {
        third->getSubscriptionManager()->retainedMessages().forEachMatching(
            topic, [&storedOnThird](const string&, const RetainedMessages::Record& record)
            {
                storedOnThird = record.m_payload == "before joining";
            });
        if (!storedOnThird) this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(storedOnThird) << "Joining node did not synchronize retained messages";
}
