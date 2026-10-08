/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/TestServers.h"

#include <mutex>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Longer than a lease, plus a step: a node that stopped is gone by then.
constexpr auto LeaseRunsOut = chrono::seconds(XMQ_ClusterTests::TestLeaseSeconds * 2 + 1);

/// Payloads a subscriber received, in order.
class Received
{
public:
    void listen(const client::SMqttClient& client)
    {
        client->onMessage([this](const SPublishMessage& message)
                          {
                              const scoped_lock lock(m_mutex);
                              m_payloads.emplace_back(message->payload());
                          });
    }

    [[nodiscard]] vector<string> payloads() const
    {
        const scoped_lock lock(m_mutex);
        return m_payloads;
    }

    [[nodiscard]] size_t count() const
    {
        return payloads().size();
    }

private:
    mutable mutex  m_mutex;
    vector<string> m_payloads;
};

string sessionOwner(const string& clientId)
{
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    const auto owner = redis.getValue(format("session_{}_owner", clientId));
    return owner.isNull() ? string() : string(owner.asString().c_str());
}

} // namespace

/**
 * Confirm that a persistent session moves to the node its client connects to, with what was queued
 * for it, and that only that node serves it afterwards.
 *
 * Setup: Two nodes. A persistent subscriber on node 0 subscribes and disconnects; three messages are
 * published to it on node 0. The subscriber connects to node 1.
 *
 * Verification: it gets the three messages, the session is node 1's - in Redis and in memory, gone
 * from node 0 - and a message published on node 0 afterwards reaches it once.
 */
TEST_F(XMQ_ClusterTests, sessionMovesToTheNodeItsClientConnectsTo)
{
    const TestCluster cluster(2);
    const string      topic = "ownership/moves";
    const string      clientId = "moving-subscriber";

    auto subscriber = cluster.connect(0, clientId, false);
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topic), SubscriptionOptions(Qos::Qos1)));
    subscriber->disconnect();

    const auto publisher = cluster.connect(0, "ownership-publisher");
    for (const auto* payload: {"one", "two", "three"})
    {
        publisher->publish(topic, payload, Qos::Qos1);
    }

    Received received;
    subscriber = make_shared<client::MqttClient>(logEngine());
    received.listen(subscriber);
    ASSERT_EQ(ReasonCode::Success, subscriber->connect(TestCluster::host(1), ConnectCredentials(clientId, "user", "secret"),
                                                       {.m_cleanSession = false}, ProtocolVersion::MqttV5));
    EXPECT_TRUE(TestCluster::waitFor([&] { return received.count() >= 3; })) << "the queued messages did not come along";
    EXPECT_EQ((vector<string> {"one", "two", "three"}), received.payloads());

    EXPECT_EQ(cluster[1]->getCluster()->nodeId(), sessionOwner(clientId));
    EXPECT_EQ(nullptr, cluster[0]->getClientSession(clientId)) << "node 0 kept the session";

    publisher->publish(topic, "four", Qos::Qos1);
    EXPECT_TRUE(TestCluster::waitFor([&] { return received.count() >= 4; }));
    this_thread::sleep_for(200ms);
    EXPECT_EQ((vector<string> {"one", "two", "three", "four"}), received.payloads()) << "a message was lost or doubled";
}

/**
 * Confirm that a client connecting to another node while still connected to the first takes its
 * session along, and the first connection is closed.
 *
 * Setup: Two nodes. A persistent subscriber connected to node 0 connects again, under the same
 * client id, to node 1.
 *
 * Verification: the first connection is closed; the second gets what is published, once.
 */
TEST_F(XMQ_ClusterTests, connectedClientIsTakenOverAcrossNodes)
{
    const TestCluster cluster(2);
    const string      topic = "ownership/takeover";
    const string      clientId = "taken-over-subscriber";

    const auto first = cluster.connect(0, clientId, false);
    first->subscribe(Destination(client::MqttClient::getTopic(topic), SubscriptionOptions(Qos::Qos1)));

    Received received;
    const auto second = make_shared<client::MqttClient>(logEngine());
    received.listen(second);
    ASSERT_EQ(ReasonCode::Success, second->connect(TestCluster::host(1), ConnectCredentials(clientId, "user", "secret"),
                                                   {.m_cleanSession = false}, ProtocolVersion::MqttV5));
    EXPECT_TRUE(TestCluster::waitFor([&] { return !first->isConnected(); })) << "the first connection stayed open";

    // Published once node 0 knows the subscription is node 1's now. Until then a message published
    // on node 0 has nowhere to go - the handover's own gap, which Cluster.md lists as open.
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(1)).contains(topic); }))
        << "node 1 did not advertise the subscription it took over";
    const auto publisher = cluster.connect(0, "takeover-publisher");
    publisher->publish(topic, "after", Qos::Qos1);
    EXPECT_TRUE(TestCluster::waitFor([&] { return received.count() >= 1; })) << "the subscription did not come along";
    this_thread::sleep_for(200ms);
    EXPECT_EQ(vector<string> {"after"}, received.payloads());
}

/**
 * Confirm that the session of a node that is gone is taken over by the node its client connects to.
 *
 * Setup: Two nodes. A persistent subscriber on node 0 subscribes and disconnects; a message is
 * published to it. Node 0 stops, and its lease runs out. The subscriber connects to node 1.
 *
 * Verification: it gets the message, and the session is node 1's.
 */
TEST_F(XMQ_ClusterTests, sessionOfANodeThatIsGoneIsTakenOver)
{
    TestCluster  cluster(2);
    const string topic = "ownership/orphan";
    const string clientId = "orphaned-subscriber";

    auto subscriber = cluster.connect(0, clientId, false);
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topic), SubscriptionOptions(Qos::Qos1)));
    subscriber->disconnect();
    cluster.connect(0, "orphan-publisher")->publish(topic, "left behind", Qos::Qos1);
    this_thread::sleep_for(100ms);

    cluster.stopNode(0);
    this_thread::sleep_for(LeaseRunsOut);

    Received received;
    subscriber = make_shared<client::MqttClient>(logEngine());
    received.listen(subscriber);
    ASSERT_EQ(ReasonCode::Success, subscriber->connect(TestCluster::host(1), ConnectCredentials(clientId, "user", "secret"),
                                                       {.m_cleanSession = false}, ProtocolVersion::MqttV5));
    EXPECT_TRUE(TestCluster::waitFor([&] { return received.count() >= 1; })) << "the message left on the node that is gone was lost";
    EXPECT_EQ(vector<string> {"left behind"}, received.payloads());
    EXPECT_EQ(cluster[1]->getCluster()->nodeId(), sessionOwner(clientId));
}
