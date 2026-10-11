/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/TestServers.h"
#include "server/Cluster/RetainedSync.h"

#include <sptk5/net/RedisConnect.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Publish a retained value, or clear it with an empty one.
void publishRetained(const client::SMqttClient& publisher, const string& topic, const string& payload)
{
    publisher->publish(topic, payload, Qos::Qos1, true);
}

/// Wait until every running node holds the payload for the topic; nothing means cleared.
bool allNodesHold(const TestCluster& cluster, const string& topic, const optional<string>& payload)
{
    return TestCluster::waitFor([&]
                                {
                                    for (size_t i = 0; i < cluster.size(); ++i)
                                    {
                                        if (cluster[i] && cluster.retained(i, topic) != payload)
                                        {
                                            return false;
                                        }
                                    }
                                    return true;
                                });
}

} // namespace

/**
 * Confirm that a retained value, its replacement and its clearing reach every node, wherever
 * they are published, without any subscriber on the other nodes.
 *
 * Setup: Three nodes, no subscribers. Publish a value on node 0, replace it from node 1, clear it
 * from node 2.
 *
 * Verification: After each step every node holds the same thing. A subscriber arriving on node 2
 * while the replacement is held is given the replacement.
 */
TEST_F(XMQ_ClusterTests, retainedMessageReplicatesWithoutSubscribers)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    publishRetained(cluster.connect(0, publisherId + "_0"), topic, "first");
    EXPECT_TRUE(allNodesHold(cluster, topic, "first")) << "A node did not store the retained publication";

    publishRetained(cluster.connect(1, publisherId + "_1"), topic, "second");
    EXPECT_TRUE(allNodesHold(cluster, topic, "second")) << "A node did not take the replacement";

    Semaphore  received;
    string     payload;
    const auto subscriber = cluster.connect(2, subscriberId);
    subscriber->onMessage([&payload, &received](const SPublishMessage& message)
                          {
                              payload.assign(message->payload());
                              received.post();
                          });
    subscriber->subscribe(topic);
    ASSERT_TRUE(received.wait_for(1s));
    EXPECT_EQ("second", payload);
    subscriber->disconnect();

    publishRetained(cluster.connect(2, publisherId + "_2"), topic, "");
    EXPECT_TRUE(allNodesHold(cluster, topic, nullopt)) << "A node did not clear its copy of the retained message";
}

/**
 * Confirm that a node joining an existing cluster receives retained state published earlier,
 * without requiring any client subscription on the joining node.
 *
 * Setup: Two nodes; publish a retained value and wait until both hold it. Then add a third node.
 *
 * Verification: The third node holds the value published before it joined.
 */
TEST_F(XMQ_ClusterTests, joiningNodeReceivesRetainedMessagesWithoutSubscribers)
{
    TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    publishRetained(cluster.connect(0, publisherId), topic, "before joining");
    ASSERT_TRUE(allNodesHold(cluster, topic, "before joining"));

    const auto third = cluster.addNode();
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster.retained(third, topic) == "before joining"; }))
        << "Joining node did not synchronize retained messages";
}

/**
 * Confirm that a retained message cleared while a node was down does not come back with it.
 *
 * Setup: Three nodes hold a retained value. Stop node 2, clear the value on node 0, then start
 * node 2 again with the copy it kept in storage.
 *
 * Verification: Node 2 drops its old copy, and the other nodes stay cleared - the old copy is
 * not handed back to them.
 */
TEST_F(XMQ_ClusterTests, retainedClearedWhileNodeWasDownStaysCleared)
{
    TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    const auto publisher = cluster.connect(0, publisherId);
    publishRetained(publisher, topic, "stale");
    ASSERT_TRUE(allNodesHold(cluster, topic, "stale"));

    cluster.stopNode(2);
    publishRetained(publisher, topic, "");
    ASSERT_TRUE(allNodesHold(cluster, topic, nullopt));

    cluster.startNode(2);
    EXPECT_TRUE(allNodesHold(cluster, topic, nullopt)) << "The restarted node kept a cleared message";

    // Long enough for a resurrected copy to have travelled; it must not appear anywhere.
    this_thread::sleep_for(500ms);
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        EXPECT_EQ(nullopt, cluster.retained(i, topic)) << TestCluster::nodeName(i) << " brought a cleared message back";
    }
}

/**
 * Confirm that a node down while a retained message was replaced takes the replacement back up.
 *
 * Setup: Three nodes hold a value. Stop node 2, replace the value on node 1, start node 2 again.
 *
 * Verification: Every node holds the replacement; node 2's older copy wins nowhere.
 */
TEST_F(XMQ_ClusterTests, retainedReplacedWhileNodeWasDownIsTakenOnRejoin)
{
    TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    publishRetained(cluster.connect(0, publisherId + "_0"), topic, "old");
    ASSERT_TRUE(allNodesHold(cluster, topic, "old"));

    cluster.stopNode(2);
    publishRetained(cluster.connect(1, publisherId + "_1"), topic, "new");
    ASSERT_TRUE(allNodesHold(cluster, topic, "new"));

    cluster.startNode(2);
    EXPECT_TRUE(allNodesHold(cluster, topic, "new")) << "The older copy of a rejoining node won";
}

/**
 * Confirm that changes made to one topic on different nodes at the same time end in one value.
 *
 * Setup: Three nodes. Publish different values to the same topic from every node at once,
 * several rounds.
 *
 * Verification: After each round all nodes hold the same value.
 */
TEST_F(XMQ_ClusterTests, concurrentRetainedChangesConverge)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    vector<client::SMqttClient> publishers;
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        publishers.push_back(cluster.connect(i, format("{}_{}", publisherId, i)));
    }

    for (auto round = 0; round < 5; ++round)
    {
        for (size_t i = 0; i < publishers.size(); ++i)
        {
            publishRetained(publishers[i], topic, format("round {} from {}", round, i));
        }

        const auto converged = TestCluster::waitFor([&]
                                                    {
                                                        const auto first = cluster.retained(0, topic);
                                                        return first && cluster.retained(1, topic) == first &&
                                                               cluster.retained(2, topic) == first;
                                                    });
        EXPECT_TRUE(converged) << "Nodes disagree after round " << round << ": '"
                               << cluster.retained(0, topic).value_or("-") << "', '"
                               << cluster.retained(1, topic).value_or("-") << "', '"
                               << cluster.retained(2, topic).value_or("-") << "'";
    }
}

/**
 * Confirm that a node joining does not replay retained messages to subscribers already served.
 *
 * Setup: Two nodes hold a retained value; a subscriber on node 0 receives it on subscribing.
 * Then a third node joins.
 *
 * Verification: The subscriber receives nothing more - the joining node's synchronisation is not
 * a publication.
 */
TEST_F(XMQ_ClusterTests, joiningNodeDoesNotReplayRetainedToSubscribers)
{
    TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    publishRetained(cluster.connect(1, publisherId), topic, "held");
    ASSERT_TRUE(allNodesHold(cluster, topic, "held"));

    atomic_int deliveries {0};
    const auto subscriber = cluster.connect(0, subscriberId);
    subscriber->onMessage([&deliveries](const SPublishMessage&) { ++deliveries; });
    subscriber->subscribe(topic);
    ASSERT_TRUE(TestCluster::waitFor([&deliveries] { return deliveries == 1; }));

    const auto third = cluster.addNode();
    ASSERT_TRUE(TestCluster::waitFor([&] { return cluster.retained(third, topic) == "held"; }));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(1, deliveries) << "The retained message was delivered again when a node joined";
}

namespace {

/// What the shared storage holds for the topic; nothing means cleared.
optional<string> stored(const string& topic)
{
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    const auto value = redis.getHashValue(cluster::RetainedSync::shardKey(RetainedMessages::shardOf(topic)), topic);
    RetainedMessages::Record record;
    if (value.isNull() || !RetainedMessages::parseShared(value.asString().c_str(), record))
    {
        return nullopt;
    }
    return record.m_payload;
}

} // namespace

/**
 * Confirm that a change committed by a node that sends it to nobody - one that died between the
 * commit and the sending - still reaches every other node.
 *
 * Setup: Three nodes. Node 0 commits its changes but sends none. A retained value is published on
 * node 0, then replaced there.
 *
 * Verification: Nodes 1 and 2 hold the value and then the replacement: they see their shard's
 * revision counter run ahead of what they have, and read the shard from the storage.
 */
TEST_F(XMQ_ClusterTests, retainedChangeCommittedButNotSentReachesEveryNode)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    cluster[0]->getCluster()->retainedSync()->simulateBroadcastLoss(true);
    const auto publisher = cluster.connect(0, publisherId);
    publishRetained(publisher, topic, "committed, not sent");
    EXPECT_TRUE(allNodesHold(cluster, topic, "committed, not sent")) << "A node missed a change that was never sent to it";

    publishRetained(publisher, topic, "");
    EXPECT_TRUE(allNodesHold(cluster, topic, nullopt)) << "A node missed a clearing that was never sent to it";
}

/**
 * Confirm that a change that arrives late - after a clearing with a greater revision - does not
 * bring the cleared message back, and that repeats change nothing.
 *
 * Setup: Three nodes hold a value; it is cleared. Node 1 is then handed the value again, with the
 * revision it had, twice - as a link that delivered it late, and repeated it, would.
 *
 * Verification: Every node stays cleared: node 1 does not believe a change to a topic it does not
 * hold, and asks the storage.
 */
TEST_F(XMQ_ClusterTests, lateRetainedChangeDoesNotBringAClearedMessageBack)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    const auto publisher = cluster.connect(0, publisherId);
    publishRetained(publisher, topic, "will be cleared");
    ASSERT_TRUE(allNodesHold(cluster, topic, "will be cleared"));
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    RetainedMessages::Record late;
    ASSERT_TRUE(RetainedMessages::parseShared(
        redis.getHashValue(cluster::RetainedSync::shardKey(RetainedMessages::shardOf(topic)), topic).asString().c_str(), late));

    publishRetained(publisher, topic, "");
    ASSERT_TRUE(allNodesHold(cluster, topic, nullopt));

    auto* retainedSync = cluster[1]->getCluster()->retainedSync();
    retainedSync->receive(topic, late);
    retainedSync->receive(topic, late);
    this_thread::sleep_for(500ms);
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        EXPECT_EQ(nullopt, cluster.retained(i, topic)) << TestCluster::nodeName(i) << " took a late change";
    }
}

/**
 * Confirm that replacements and clearings of one topic made on every node at once end in one state,
 * on every node and in the storage.
 *
 * Setup: Three nodes. For several rounds every node changes the same topic at once: some rounds
 * replace it everywhere, some clear it on one node while the others replace it, some clear it on
 * two.
 *
 * Verification: After each round all nodes hold the same thing, and it is what the storage holds.
 */
TEST_F(XMQ_ClusterTests, concurrentReplacementsAndClearingsConverge)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();

    vector<client::SMqttClient> publishers;
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        publishers.push_back(cluster.connect(i, format("{}_{}", publisherId, i)));
    }

    for (auto round = 0; round < 9; ++round)
    {
        // Round 0, 3, 6: everyone replaces. 1, 4, 7: node 0 clears. 2, 5, 8: nodes 0 and 1 clear.
        const size_t clearing = static_cast<size_t>(round % 3);
        for (size_t i = 0; i < publishers.size(); ++i)
        {
            publishRetained(publishers[i], topic, i < clearing ? "" : format("round {} from {}", round, i));
        }

        const auto converged = TestCluster::waitFor([&]
                                                    {
                                                        const auto inStorage = stored(topic);
                                                        for (size_t i = 0; i < cluster.size(); ++i)
                                                        {
                                                            if (cluster.retained(i, topic) != inStorage)
                                                            {
                                                                return false;
                                                            }
                                                        }
                                                        return true;
                                                    });
        EXPECT_TRUE(converged) << "Round " << round << ": storage '" << stored(topic).value_or("-") << "', nodes '"
                               << cluster.retained(0, topic).value_or("-") << "', '"
                               << cluster.retained(1, topic).value_or("-") << "', '"
                               << cluster.retained(2, topic).value_or("-") << "'";
    }
}
