/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "test/TestServers.h"

#include <sptk5/net/RedisCommand.h>

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

/// Hold the coordinator key for a node that does not exist, so that no node can get a lease.
void occupyCoordinator(const string& owner)
{
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    RedisCommand command("SET", "cluster:coordinator");
    command.emplace_back("999999 " + owner);
    command.emplace_back("PX");
    command.emplace_back("60000");
    vector<Variant> results;
    redis.executeCommand(command, results);
}

void releaseCoordinator()
{
    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    (void) redis.deleteKeys({"cluster:coordinator"});
}

} // namespace

/**
 * Confirm that the first node of a cluster becomes its coordinator and every node gets a lease.
 *
 * Setup: Three nodes, joined in order.
 *
 * Verification: node 0 is coordinator on every node's view, in term 1 or later; the members are in
 * the order they joined; every node is online and takes clients.
 */
TEST_F(XMQ_ClusterTests, firstNodeCoordinatesAndEveryNodeGetsALease)
{
    const TestCluster cluster(3);

    EXPECT_TRUE(coordinatorOf(cluster, 0).isCoordinator());
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, i).coordinatorName() == TestCluster::nodeName(0); }))
            << TestCluster::nodeName(i) << " does not see node 0 as coordinator";
        EXPECT_GE(coordinatorOf(cluster, i).term(), 1);
        EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[i]->getCluster()->isOnline(); }))
            << TestCluster::nodeName(i) << " has no lease";
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
 * Confirm that a node without a lease serves no clients, and serves them again once it has one.
 *
 * Setup: Two nodes and a client on node 1. Stop node 0 and hold the coordinator key for a node
 * that does not exist, so node 1 can neither renew its lease nor take over.
 *
 * Verification: once its lease has run out, node 1 is offline, its client has been disconnected and
 * a new client is refused as "server unavailable". Freed, the key goes to node 1, which takes clients again.
 */
TEST_F(XMQ_ClusterTests, nodeWithoutLeaseServesNoClients)
{
    TestCluster cluster(2);
    const auto  client = cluster.connect(1, "client-of-node-1", false);
    ASSERT_TRUE(client->isConnected());

    cluster.stopNode(0);
    occupyCoordinator("ghost-node");

    EXPECT_TRUE(TestCluster::waitFor([&] { return !cluster[1]->getCluster()->isOnline(); }, Handover))
        << "node 1 kept serving without a lease";
    EXPECT_TRUE(TestCluster::waitFor([&] { return !client->isConnected(); }, Handover))
        << "the client of an offline node stayed connected";

    client::MqttClient refused(ServerTests_Suite::logEngine());
    EXPECT_EQ(ReasonCode::ServerUnavailable,
              refused.connect(TestCluster::host(1), ConnectCredentials("refused-client", "user", "secret"), {},
                              ProtocolVersion::MqttV5));

    releaseCoordinator();
    EXPECT_TRUE(TestCluster::waitFor([&] { return coordinatorOf(cluster, 1).isCoordinator(); }, Handover));
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
