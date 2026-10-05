/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Check that cluster peers keep one effective entry per local filter, even when multiple
 * persistent client sessions use it. Removing one client must retain the entry, and disconnecting
 * the last client must retain it while its persistent session is offline. A separate filter is
 * unsubscribed explicitly to verify that removal propagates when no local client still uses it.
 */
TEST_F(XMQ_ClusterTests, effectiveSubscriptionsAreSharedAndDeduplicated)
{
    auto [primary, secondary] = makeClusterOfTwoNodes();
    const auto [clientId1, clientId2, sharedFilter] = makeTestNames();
    const string removedFilter = sharedFilter + "/removed";

    auto connectSubscriber = [](const Host& host, const string& id)
    {
        auto client = make_shared<client::MqttClient>();
        EXPECT_EQ(ReasonCode::Success,
                  client->connect(host, ConnectCredentials(id, "user", "secret"),
                                  {.m_cleanSession = false}, ProtocolVersion::MqttV5));
        return client;
    };
    auto first = connectSubscriber(m_primaryServerHost, clientId1);
    auto second = connectSubscriber(m_primaryServerHost, clientId2);
    ASSERT_TRUE(first->isConnected());
    ASSERT_TRUE(second->isConnected());
    first->subscribe(sharedFilter);
    second->subscribe(sharedFilter);
    first->subscribe(removedFilter);

    auto hasFilter = [&]
    {
        const auto local = primary->getCluster()->getNodeSubscriptions("primary");
        const auto filters = secondary->getCluster()->getNodeSubscriptions("primary");
        return local.contains(sharedFilter) && local.contains(removedFilter) &&
               filters.contains(sharedFilter) && filters.contains(removedFilter);
    };
    for (int i = 0; i < 100 && !hasFilter(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(hasFilter()) << "Subscription snapshot did not reach the peer";

    first->unsubscribe(Destination(client::MqttClient::getTopic(sharedFilter)));
    first->unsubscribe(Destination(client::MqttClient::getTopic(removedFilter)));
    auto removedExplicitly = [&]
    {
        const auto filters = secondary->getCluster()->getNodeSubscriptions("primary");
        return filters.contains(sharedFilter) && !filters.contains(removedFilter);
    };
    for (int i = 0; i < 100 && !removedExplicitly(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(removedExplicitly()) << "Unsubscribe did not update the shared filter set";

    second->disconnect();
    const auto persistedOffline = [&]
    {
        return secondary->getCluster()->getNodeSubscriptions("primary").contains(sharedFilter);
    };
    for (int i = 0; i < 100 && !persistedOffline(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(persistedOffline()) << "Offline persistent session lost its effective subscription";
}

namespace {

/// Does a node know that another one has a local subscriber for the filter?
bool knows(const TestCluster& cluster, const size_t node, const size_t subscriberNode, const string& filter)
{
    return cluster[node]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(subscriberNode)).contains(filter);
}

} // namespace

/**
 * Confirm that a subscription and its removal reach every other node of a cluster of three.
 *
 * Setup: Three nodes; a client on node 1 subscribes, then unsubscribes.
 * Verification: Nodes 0 and 2 both list the filter for node 1, then both drop it.
 */
TEST_F(XMQ_ClusterTests, subscriptionsReachEveryNode)
{
    const TestCluster cluster(3);
    const auto [clientId, unused, filter] = makeTestNames();

    const auto client = cluster.connect(1, clientId);
    client->subscribe(filter);
    EXPECT_TRUE(TestCluster::waitFor([&] { return knows(cluster, 0, 1, filter) && knows(cluster, 2, 1, filter); }))
        << "A subscription did not reach every node";

    client->unsubscribe(Destination(client::MqttClient::getTopic(filter)));
    EXPECT_TRUE(TestCluster::waitFor([&] { return !knows(cluster, 0, 1, filter) && !knows(cluster, 2, 1, filter); }))
        << "An unsubscribe did not reach every node";
}

/**
 * Confirm that a node joining a cluster learns the subscriptions made before it joined.
 *
 * Setup: Two nodes; a client on node 0 subscribes. Then a third node joins.
 * Verification: The third node lists the filter for node 0.
 */
TEST_F(XMQ_ClusterTests, joiningNodeLearnsExistingSubscriptions)
{
    TestCluster cluster(2);
    const auto [clientId, unused, filter] = makeTestNames();

    const auto client = cluster.connect(0, clientId);
    client->subscribe(filter);
    ASSERT_TRUE(TestCluster::waitFor([&] { return knows(cluster, 1, 0, filter); }));

    const auto third = cluster.addNode();
    EXPECT_TRUE(TestCluster::waitFor([&] { return knows(cluster, third, 0, filter); }))
        << "A joining node was not sent the existing subscriptions";
}

/**
 * Confirm that the subscription of a persistent session leaves the other nodes when the session
 * expires, as an unsubscribe would.
 *
 * Setup: Two nodes; a persistent client with a one-second session expiry subscribes on node 0
 * and disconnects.
 * Verification: Node 1 lists the filter while the session is held, and drops it once it expires.
 */
TEST_F(XMQ_ClusterTests, expiredSessionWithdrawsItsSubscription)
{
    const TestCluster cluster(2);
    const auto [clientId, unused, filter] = makeTestNames();

    auto       client = make_shared<client::MqttClient>(logEngine());
    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::SessionExpiryInterval, 1);
    ASSERT_EQ(ReasonCode::Success,
              client->connect(TestCluster::host(0), ConnectCredentials(clientId, "user", "secret"),
                              {.m_cleanSession = false}, ProtocolVersion::MqttV5, properties));
    client->subscribe(filter);
    ASSERT_TRUE(TestCluster::waitFor([&] { return knows(cluster, 1, 0, filter); }));

    client->disconnect();
    EXPECT_TRUE(knows(cluster, 1, 0, filter)) << "A disconnected persistent session lost its subscription at once";
    EXPECT_TRUE(TestCluster::waitFor([&] { return !knows(cluster, 1, 0, filter); }, 5s))
        << "An expired session's subscription stayed on the other node";
}

/**
 * Confirm that a burst of subscriptions reaches the other node whole.
 *
 * Setup: Two nodes; a client on node 0 subscribes to 300 filters as fast as it can.
 * Verification: Node 1 lists all 300 for node 0.
 */
TEST_F(XMQ_ClusterTests, subscriptionBurstReachesThePeerWhole)
{
    const TestCluster cluster(2);
    const auto [clientId, unused, topic] = makeTestNames();
    constexpr auto filterCount = 300;

    const auto client = cluster.connect(0, clientId);
    for (auto i = 0; i < filterCount; ++i)
    {
        client->subscribe(format("{}/{}", topic, i));
    }
    const auto allKnown = [&]
    {
        const auto filters = cluster[1]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(0));
        return ranges::count_if(filters, [&topic](const string& f) { return f.starts_with(topic + "/"); }) == filterCount;
    };
    EXPECT_TRUE(TestCluster::waitFor(allKnown, 5s)) << "A burst of subscriptions did not reach the peer whole";
}
