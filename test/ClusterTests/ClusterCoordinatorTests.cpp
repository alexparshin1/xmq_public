/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include <algorithm>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

cluster::Coordinator& coordinatorOf(const TestCluster& cluster, const size_t index)
{
    auto* coordinator = cluster[index]->getCluster()->coordinator();
    if (coordinator == nullptr)
    {
        throw Exception(format("{} is not in a cluster", TestCluster::nodeName(index)));
    }
    return *coordinator;
}

/// Longer than a lease, plus a step for the next coordinator to take over.
constexpr auto Handover = chrono::seconds(XMQ_ClusterTests::TestLeaseSeconds * 2 + 1);

} // namespace

/**
 * Confirm that the first node of a cluster becomes its coordinator and every node is online.
 *
 * Setup: Three nodes, joined in order.
 *
 * Verification: node 0 is coordinator on every node's view, in term 1 or later; the members are in
 * the order they joined; every node is online and takes clients.
 */
TEST_F(XMQ_ClusterTests, firstNodeCoordinatesAndEveryNodeIsOnline)
{
    const TestCluster cluster(3);

    EXPECT_TRUE(coordinatorOf(cluster, 0).isCoordinator());
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, i).coordinatorName() == TestCluster::nodeName(0); }))
            << TestCluster::nodeName(i) << " does not see node 0 as coordinator";
        EXPECT_GE(coordinatorOf(cluster, i).term(), 1);
        EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[i]->getCluster()->isOnline(); }))
            << TestCluster::nodeName(i) << " is offline";
    }
    EXPECT_EQ((vector<string> {TestCluster::nodeName(0), TestCluster::nodeName(1), TestCluster::nodeName(2)}),
              coordinatorOf(cluster, 0).members());

    const auto client = cluster.connect(2, "lease-holder-client");
    EXPECT_TRUE(client->isConnected());
}

/**
 * Confirm that when the coordinator goes, the next node in order takes over in a new term, and that
 * the former coordinator, back again, is not coordinator and goes to the end of the order.
 *
 * Setup: Three nodes. Stop node 0, the coordinator; later start it again.
 *
 * Verification: node 1 becomes coordinator in a higher term; node 2 follows it and keeps serving.
 * Restarted, node 0 sees node 1 as coordinator and is last among the members.
 */
TEST_F(XMQ_ClusterTests, nextNodeInOrderSucceedsTheCoordinator)
{
    TestCluster cluster(3);
    const auto  firstTerm = coordinatorOf(cluster, 0).term();

    cluster.stopNode(0);
    EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, 1).isCoordinator(); }, Handover))
        << "node 1 did not take over";
    EXPECT_GT(coordinatorOf(cluster, 1).term(), firstTerm);
    EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, 2).coordinatorName() == TestCluster::nodeName(1); }));
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[2]->getCluster()->isOnline(); }, Handover));

    cluster.startNode(0);
    EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, 0).coordinatorName() == TestCluster::nodeName(1); }, Handover));
    EXPECT_FALSE(coordinatorOf(cluster, 0).isCoordinator());
    EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, 1).members().back() == TestCluster::nodeName(0); }, Handover))
        << "the former coordinator kept its place in the order";
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getCluster()->isOnline(); }, Handover));
}

/**
 * Confirm that a change of coordinator is invisible to clients.
 *
 * Setup: Three nodes and a client on node 2. Stop node 0, the coordinator.
 *
 * Verification: node 1 takes over, and all the while node 2 stays online and its client stays connected.
 */
TEST_F(XMQ_ClusterTests, coordinatorChangeDoesNotDisturbClients)
{
    TestCluster cluster(3);
    const auto  client = cluster.connect(2, "client-of-node-2", false);
    ASSERT_TRUE(client->isConnected());

    cluster.stopNode(0);
    bool stayedOnline = true;
    EXPECT_TRUE(TestCluster::waitFor([&]
                                     {
                                         stayedOnline = stayedOnline && cluster[2]->getCluster()->isOnline() &&
                                                        client->isConnected();
                                         return coordinatorOf(cluster, 1).isCoordinator();
                                     },
                                     Handover));
    EXPECT_TRUE(stayedOnline) << "node 2 or its client noticed the coordinator change";
    EXPECT_TRUE(client->isConnected());
}

/**
 * Confirm that a node that cannot reach the shared storage stops serving clients once its lease
 * has run out, and serves them again when the storage is back.
 *
 * Setup: Two nodes and a client on node 1. Node 1 loses Redis.
 *
 * Verification: node 1 goes offline, its client is disconnected and a new client is refused as
 * "server unavailable". With Redis back, node 1 is online and takes the client again.
 */
TEST_F(XMQ_ClusterTests, nodeWithoutStorageServesNoClients)
{
    TestCluster cluster(2);
    const auto  client = cluster.connect(1, "client-of-node-1", false);
    ASSERT_TRUE(client->isConnected());

    coordinatorOf(cluster, 1).simulateStorageLoss(true);
    EXPECT_TRUE(TestCluster::waitFor([&] { return !cluster[1]->getCluster()->isOnline(); }, Handover))
        << "node 1 kept serving without the storage";
    EXPECT_TRUE(TestCluster::waitFor([&] { return !client->isConnected(); }, Handover))
        << "the client of an offline node stayed connected";

    client::MqttClient refused(ServerTests_Suite::logEngine());
    EXPECT_EQ(ReasonCode::ServerUnavailable,
              refused.connect(TestCluster::host(1), ConnectCredentials("refused-client", "user", "secret"), {},
                              ProtocolVersion::MqttV5));

    coordinatorOf(cluster, 1).simulateStorageLoss(false);
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[1]->getCluster()->isOnline(); }, Handover));
    const auto again = cluster.connect(1, "client-of-node-1", false);
    EXPECT_TRUE(again->isConnected());
}

/**
 * Confirm that a cluster admits at most ten nodes.
 *
 * Setup: Two nodes, and eight more members recorded for nodes that are down - they still count.
 * Start another node and ask it to join.
 *
 * Verification: the join is refused, the node is not a member and has no link.
 */
TEST_F(XMQ_ClusterTests, eleventhNodeIsRefused)
{
    TestCluster cluster(2);
    for (size_t i = 2; i < cluster::Coordinator::MaxMembers; ++i)
    {
        ASSERT_TRUE(coordinatorOf(cluster, 0).admit(format("down-node-{}", i)));
    }
    EXPECT_FALSE(coordinatorOf(cluster, 0).admit("one-too-many"));

    const auto extra = createNode("extra-node", static_cast<uint16_t>(TestCluster::FirstPort + 5), true);
    extra->attachToCluster(Host("localhost", static_cast<uint16_t>(TestCluster::FirstPort + 7000)));

    const auto members = coordinatorOf(cluster, 0).members();
    EXPECT_EQ(cluster::Coordinator::MaxMembers, members.size());
    EXPECT_EQ(members.end(), ranges::find(members, "extra-node"));
    EXPECT_EQ(0u, cluster[0]->getCluster()->getConnectedNodeCount("extra-node"));
    stopNode("extra-node");
}

/**
 * Confirm that a node that leaves the cluster frees its place among the members.
 *
 * Setup: Two nodes; node 1 detaches.
 *
 * Verification: node 1 is no longer a member; node 0 is still coordinator.
 */
TEST_F(XMQ_ClusterTests, nodeThatLeavesFreesItsPlace)
{
    const TestCluster cluster(2);
    cluster[1]->detachFromCluster();

    EXPECT_EQ(vector<string> {TestCluster::nodeName(0)}, coordinatorOf(cluster, 0).members());
    EXPECT_TRUE(coordinatorOf(cluster, 0).isCoordinator());
}
