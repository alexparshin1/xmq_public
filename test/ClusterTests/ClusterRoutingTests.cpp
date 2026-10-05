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

namespace {

/// Wait until a node has learned that another one has a local subscriber for the filter.
bool waitUntilKnown(const TestCluster& cluster, const size_t node, const size_t subscriberNode, const string& filter,
                    const bool known = true)
{
    return TestCluster::waitFor([&]
                                {
                                    return cluster[node]->getCluster()
                                               ->getNodeSubscriptions(TestCluster::nodeName(subscriberNode))
                                               .contains(filter) == known;
                                });
}

/// Forwarded publications a node has received from the others.
uint64_t forwarded(const TestCluster& cluster, const size_t node)
{
    return cluster[node]->getCluster()->forwardedMessagesReceived();
}

/// A client counting what it receives.
struct CountingClient
{
    client::SMqttClient client;
    atomic_int          received {0};
};

unique_ptr<CountingClient> countingClient(const TestCluster& cluster, const size_t node, const string& clientId)
{
    auto counting = make_unique<CountingClient>();
    counting->client = cluster.connect(node, clientId);
    counting->client->onMessage([received = &counting->received](const SPublishMessage&) { ++*received; });
    return counting;
}

} // namespace

/**
 * Confirm that a publication goes only to the nodes whose clients subscribe to it.
 *
 * Setup: Three nodes; one subscriber on node 1. A publisher on node 0 publishes to its topic.
 * Verification: The subscriber gets the message, node 1 was forwarded it once, and node 2 - with
 * no subscriber - was forwarded nothing.
 */
TEST_F(XMQ_ClusterTests, publicationGoesOnlyToNodesWithSubscribers)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    const auto subscriber = countingClient(cluster, 1, subscriberId);
    subscriber->client->subscribe(topic);
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, topic));

    cluster.connect(0, publisherId)->publish(topic, "to node 1 only");
    EXPECT_TRUE(TestCluster::waitFor([&] { return subscriber->received == 1; }));
    this_thread::sleep_for(200ms);
    EXPECT_EQ(1U, forwarded(cluster, 1));
    EXPECT_EQ(0U, forwarded(cluster, 2)) << "A node without subscribers was sent the publication";
}

/**
 * Confirm that a node is sent one copy of a publication however many of its filters match it.
 *
 * Setup: Two nodes; on node 1 one client subscribes to "<topic>/+" and another to "<topic>/#".
 * A publisher on node 0 publishes to "<topic>/x".
 * Verification: Node 1 was forwarded the publication once, and each of its clients got it once.
 */
TEST_F(XMQ_ClusterTests, overlappingFiltersForwardOneCopy)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    const auto plus = countingClient(cluster, 1, subscriberId + "_plus");
    const auto hash = countingClient(cluster, 1, subscriberId + "_hash");
    plus->client->subscribe(topic + "/+");
    hash->client->subscribe(topic + "/#");
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, topic + "/+"));
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, topic + "/#"));

    cluster.connect(0, publisherId)->publish(topic + "/x", "matches both filters");
    EXPECT_TRUE(TestCluster::waitFor([&] { return plus->received == 1 && hash->received == 1; }));
    this_thread::sleep_for(200ms);
    EXPECT_EQ(1U, forwarded(cluster, 1)) << "A node matched by two filters was sent two copies";
    EXPECT_EQ(1, plus->received);
    EXPECT_EQ(1, hash->received);
}

/**
 * Confirm that a shared subscription spread over two nodes gets each message once in the cluster.
 *
 * Setup: Two nodes, a member of "$share/<group>/<topic>" on each. Publishers on both nodes publish
 * 20 messages each.
 * Verification: The two members received 40 messages between them - none lost, none twice.
 */
TEST_F(XMQ_ClusterTests, sharedSubscriptionDeliversOncePerCluster)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();
    const auto filter = "$share/group/" + topic;
    constexpr auto perPublisher = 20;

    const auto member0 = countingClient(cluster, 0, subscriberId + "_0");
    const auto member1 = countingClient(cluster, 1, subscriberId + "_1");
    member0->client->subscribe(filter);
    member1->client->subscribe(filter);
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, filter));
    ASSERT_TRUE(waitUntilKnown(cluster, 1, 0, filter));

    // Kept until the end: a client destroyed straight after publishing takes what it has not sent
    // yet with it, and that reads as messages the cluster lost.
    vector<client::SMqttClient> publishers;
    for (size_t node = 0; node < cluster.size(); ++node)
    {
        const auto& publisher = publishers.emplace_back(cluster.connect(node, format("{}_{}", publisherId, node)));
        for (auto i = 0; i < perPublisher; ++i)
        {
            publisher->publish(topic, format("message {} from node {}", i, node));
        }
    }

    const auto total = [&] { return member0->received + member1->received; };
    EXPECT_TRUE(TestCluster::waitFor([&] { return total() >= 2 * perPublisher; }));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(2 * perPublisher, total()) << "members got " << member0->received << " and " << member1->received;
}

/**
 * Confirm that once the last subscriber on a node goes, publications stop being sent there.
 *
 * Setup: Two nodes; a subscriber on node 1 receives one publication from node 0, then unsubscribes.
 * Verification: After node 0 learns of the unsubscribe, a second publication is not forwarded.
 */
TEST_F(XMQ_ClusterTests, unsubscribeStopsForwarding)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    const auto subscriber = countingClient(cluster, 1, subscriberId);
    subscriber->client->subscribe(topic);
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, topic));

    const auto publisher = cluster.connect(0, publisherId);
    publisher->publish(topic, "first");
    ASSERT_TRUE(TestCluster::waitFor([&] { return subscriber->received == 1; }));
    ASSERT_EQ(1U, forwarded(cluster, 1));

    subscriber->client->unsubscribe(Destination(client::MqttClient::getTopic(topic)));
    ASSERT_TRUE(waitUntilKnown(cluster, 0, 1, topic, false));

    publisher->publish(topic, "second");
    this_thread::sleep_for(300ms);
    EXPECT_EQ(1U, forwarded(cluster, 1)) << "A node with no subscriber left was still sent publications";
    EXPECT_EQ(1, subscriber->received);
}
