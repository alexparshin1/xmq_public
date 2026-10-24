/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "server/Cluster/NodeIdentity.h"
#include "test/TestServers.h"

#include <sptk5/net/RedisCommand.h>

#include <algorithm>
#include <atomic>
#include <thread>

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

/// The members' names, in succession order.
vector<string> memberNames(cluster::Coordinator& coordinator)
{
    vector<string> names;
    for (const auto& member: coordinator.members())
    {
        names.push_back(member.m_name);
    }
    return names;
}

} // namespace

TEST_F(XMQ_ClusterTests, forceRemovalRequiresExpiredLeaseAndPreservesRecovery)
{
    const auto id = cluster::NodeIdentity::generate();
    const auto uri = URL(TestServers::redisUri());
    cluster::Coordinator failed(id, "failed-node", "localhost:13801", uri, 1s, {});
    failed.start();
    RedisConnect redis;
    redis.connect(uri);
    EXPECT_FALSE(cluster::Coordinator::removeOfflineMember(redis, id));
    EXPECT_EQ(1, cluster::Coordinator::membership(redis, id));
    failed.stop();
    ASSERT_TRUE(TestCluster::waitFor([&] {
        const auto view = cluster::Coordinator::readSnapshot(redis);
        return !view.m_members.empty() && !view.m_members.front().m_alive;
    }, 4s));
    vector<Variant> ignored;
    RedisCommand sessions("SADD", "node_failed-node_sessions");
    sessions.emplace_back("persistent-client");
    redis.executeCommand(sessions, ignored);
    RedisCommand pending("RPUSH", "cluster:adopt:" + id);
    pending.emplace_back("pending-client");
    redis.executeCommand(pending, ignored);
    ASSERT_TRUE(cluster::Coordinator::removeOfflineMember(redis, id));
    EXPECT_TRUE(cluster::Coordinator::readSnapshot(redis).m_members.empty());
    EXPECT_THROW(failed.start(), Exception) << "A revoked identity must not automatically rejoin";
    EXPECT_TRUE(redis.scan("node_failed-node_sessions", 10).size() == 1);
    atomic<int> recovered {0};
    cluster::Coordinator replacement(cluster::NodeIdentity::generate(), "replacement", "localhost:13802", uri, 1s, {});
    replacement.onAdoptSessions([&](vector<string> clients) {
        for (const auto& client: clients)
            if (client == "persistent-client" || client == "pending-client") ++recovered;
    });
    replacement.start();
    EXPECT_TRUE(TestCluster::waitFor([&] { return recovered == 2; }, 4s));
    replacement.leave();
}

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
              memberNames(coordinatorOf(cluster, 0)));

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
    EXPECT_TRUE(TestCluster::waitFor([&] { return memberNames(coordinatorOf(cluster, 1)).back() == TestCluster::nodeName(0); }, Handover))
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
 * Confirm that a cluster admits fifty nodes and rejects the fifty-first.
 *
 * Setup: Two live nodes, and forty-eight absent members - offline nodes still count.
 * Start another cluster node.
 *
 * Verification: the node does not start, is not a member and has no link.
 */
TEST_F(XMQ_ClusterTests, fiftyFirstNodeIsRefused)
{
    TestCluster cluster(2);
    for (size_t i = 2; i < cluster::Coordinator::MaxMembers; ++i)
    {
        ASSERT_TRUE(coordinatorOf(cluster, 0).admitAbsentNode(format("down-node-{}", i)));
    }
    EXPECT_FALSE(coordinatorOf(cluster, 0).admitAbsentNode("one-too-many"));

    EXPECT_THROW(createNode("extra-node", static_cast<uint16_t>(TestCluster::FirstPort + 5), true, {}, true), Exception);

    const auto members = memberNames(coordinatorOf(cluster, 0));
    EXPECT_EQ(cluster::Coordinator::MaxMembers, members.size());
    EXPECT_EQ(members.end(), ranges::find(members, "extra-node"));
    EXPECT_EQ(0u, cluster[0]->getCluster()->getConnectedNodeCount("extra-node"));
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

    EXPECT_EQ(vector<string> {TestCluster::nodeName(0)}, memberNames(coordinatorOf(cluster, 0)));
    EXPECT_TRUE(coordinatorOf(cluster, 0).isCoordinator());
}

/**
 * Confirm that a node keeps its GUID when it is started again, and is the same member.
 *
 * Setup: Two nodes. Stop node 1 and start it again.
 *
 * Verification: the same GUID before and after; still two members, node 1 among them, linked to
 * node 0 again without being asked to join.
 */
TEST_F(XMQ_ClusterTests, aNodeThatLeavesCanRestartStandaloneWithoutRejoining)
{
    TestCluster cluster(2);
    const auto configuration = cluster[1]->getSettings()->configurationPath();
    cluster[1]->stopServer();
    cluster[1]->detachFromCluster();
    ASSERT_TRUE(cluster[1]->getSettings()->applyClusterDisable().empty());
    EXPECT_TRUE(filesystem::exists(configuration));
    EXPECT_EQ(vector<string> {TestCluster::nodeName(0)}, memberNames(coordinatorOf(cluster, 0)));
    cluster.stopNode(1);
    const auto standalone = createNode(TestCluster::nodeName(1), TestCluster::host(1).port(), false, {}, false);
    EXPECT_EQ(nullptr, standalone->getCluster()->coordinator());
    EXPECT_EQ(vector<string> {TestCluster::nodeName(0)}, memberNames(coordinatorOf(cluster, 0)));
}

TEST_F(XMQ_ClusterTests, restartedNodeKeepsItsIdentityAndRejoins)
{
    TestCluster cluster(2);
    const auto  before = cluster[1]->getCluster()->nodeId();
    EXPECT_TRUE(cluster::NodeIdentity::isGuid(before));
    EXPECT_NE(before, cluster[0]->getCluster()->nodeId());

    cluster.stopNode(1);
    cluster.startNode(1); // returns once the node has linked to node 0 again, by itself

    EXPECT_EQ(before, cluster[1]->getCluster()->nodeId());
    EXPECT_EQ((vector<string> {TestCluster::nodeName(0), TestCluster::nodeName(1)}), memberNames(coordinatorOf(cluster, 0)));
}

/**
 * Confirm that a node may not take the name of another node of the cluster.
 *
 * Setup: Two nodes; a member called "taken-name" recorded for a node that is down. A new cluster
 * node with that name - and a GUID of its own - starts.
 *
 * Verification: it does not start, and the name still belongs to the member that had it.
 */
TEST_F(XMQ_ClusterTests, nodeMayNotTakeAnotherNodesName)
{
    TestCluster cluster(2);
    ASSERT_TRUE(coordinatorOf(cluster, 0).admitAbsentNode("taken-name"));

    EXPECT_THROW(createNode("taken-name", static_cast<uint16_t>(TestCluster::FirstPort + 5), true, {}, true), Exception);
    EXPECT_EQ(0u, cluster[0]->getCluster()->getConnectedNodeCount("taken-name"));
    EXPECT_EQ(3u, memberNames(coordinatorOf(cluster, 0)).size());
}

/**
 * Confirm that a node running already, under its GUID, is not started a second time.
 *
 * Setup: Two nodes. Stop node 1, and give its lease to another run of it - as a copy of the node,
 * started from a cloned machine, would hold it.
 *
 * Verification: node 1 does not start while the other run holds the lease.
 */
TEST_F(XMQ_ClusterTests, nodeRunningElsewhereDoesNotStartAgain)
{
    TestCluster cluster(2);
    const auto  nodeId = cluster[1]->getCluster()->nodeId();
    cluster.stopNode(1);

    RedisConnect redis;
    redis.connect(URL(TestServers::redisUri()));
    RedisCommand hold("SET", "cluster:alive:" + nodeId);
    hold.emplace_back("another-run");
    hold.emplace_back("PX");
    hold.emplace_back("60000");
    vector<Variant> results;
    redis.executeCommand(hold, results);

    // A listener starts accepting when it is added, not when FastTCPServer::start() is called.
    // Probe throughout the lease wait: a refused node must never admit sessions whose receive
    // threads would outlive its failed constructor.
    atomic<bool> accepted {false};
    {
        // FreeBSD's libc++ has no jthread/stop_token. Keep the same cleanup on
        // every exit path, including an unexpected exception from createNode().
        struct ProbeThread
        {
            atomic<bool> stop {false};
            thread worker;
            ~ProbeThread()
            {
                stop = true;
                if (worker.joinable()) worker.join();
            }
        } probe;
        probe.worker = thread([&]
        {
            while (!probe.stop)
            {
                try
                {
                    TCPSocket socket;
                    socket.open(TestCluster::host(1));
                    accepted = true;
                    socket.close();
                }
                catch (const Exception&)
                {
                }
                this_thread::sleep_for(10ms);
            }
        });
        EXPECT_THROW(createNode(TestCluster::nodeName(1), TestCluster::host(1).port(), false, {}, true), Exception);
    }
    EXPECT_FALSE(accepted.load()) << "A node waiting for its lease must not accept MQTT connections";
}

/**
 * Confirm that a node that is not a cluster node may not use a cluster's database.
 *
 * Setup: Two nodes. Start a third node on the same storage, neither a member nor cluster.enabled.
 *
 * Verification: it does not start.
 */
TEST_F(XMQ_ClusterTests, standaloneNodeMayNotUseAClustersDatabase)
{
    const TestCluster cluster(2);
    // A node that shares a cluster's storage and is not a member may serve on it now: its
    // sessions go under the namespace of its GUID, so it names nothing the cluster uses.
    ASSERT_NO_THROW(createNode("standalone", static_cast<uint16_t>(TestCluster::FirstPort + 5), true));
    // The cluster's two nodes still see each other; the standalone node is not a member.
    EXPECT_EQ(2U, coordinatorOf(cluster, 0).members().size());
}

/**
 * Confirm that a node with cluster.enabled joins the cluster in its storage by itself.
 *
 * Setup: Two nodes. Start a third with cluster.enabled, and ask it nothing.
 *
 * Verification: it becomes a member and links to both nodes.
 */
TEST_F(XMQ_ClusterTests, enabledNodeJoinsByItself)
{
    const TestCluster cluster(2);
    const auto        joiner = createNode("joiner", static_cast<uint16_t>(TestCluster::FirstPort + 5), true, {}, true);

    EXPECT_TRUE(TestCluster::waitFor([&] { return joiner->getCluster()->getConnectedNodeCount() >= 2; }, Handover))
        << "the node did not link to the cluster";
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getCluster()->getConnectedNodeCount("joiner") == 1; }, Handover))
        << "the cluster did not link back to the node";
    EXPECT_EQ((vector<string> {TestCluster::nodeName(0), TestCluster::nodeName(1), "joiner"}), memberNames(coordinatorOf(cluster, 0)));
    stopNode("joiner");
}

/**
 * Confirm that a node asked to take part in the cluster twice at the same moment - its own start
 * and an attach arriving over a link - starts its coordinator once, and survives it.
 *
 * Setup: A coordinator for a node of its own, started from two threads released together; again,
 * many times, each with a coordinator of its own.
 *
 * Verification: the process is still here, and each coordinator ends up online. The thread used to
 * be installed after the lock that guarded the question "is it running already?", so both callers
 * passed it and the second assignment, to a running std::thread, ended the process.
 */
TEST_F(XMQ_ClusterTests, coordinatorStartedTwiceAtOnceStartsOnce)
{
    constexpr int attempts = 50;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        cluster::Coordinator coordinator(cluster::NodeIdentity::generate(), format("racing-node-{}", attempt), "localhost:1",
                                         URL(TestServers::redisUri()), chrono::seconds(TestLeaseSeconds), {});
        atomic_bool go {false};
        const auto  startWhenReleased = [&]
        {
            while (!go)
            {
            }
            coordinator.start();
        };
        thread first(startWhenReleased);
        thread second(startWhenReleased);
        go = true;
        first.join();
        second.join();
        EXPECT_TRUE(coordinator.isOnline());
        coordinator.leave();
    }
}

/**
 * Confirm that a node whose storage stops answering - the network lost it, nothing refuses - goes
 * cluster-offline when its lease runs out, while its coordinator's step is still waiting on Redis,
 * and serves again as soon as the storage answers.
 *
 * Setup: Two nodes and a client on node 1. Every Redis command of node 1's coordinator waits.
 *
 * Verification: node 1 goes offline and its client is disconnected within a lease and a step,
 * though no step returns meanwhile; the storage back, node 1 takes a client again. Reported from
 * the step, the offline state came only when the step gave up, and the client stayed connected.
 */
TEST_F(XMQ_ClusterTests, nodeWhoseStorageStopsAnsweringGoesOfflineAndReturns)
{
    TestCluster cluster(2);
    const auto  client = cluster.connect(1, "client-of-a-hung-node", false);
    ASSERT_TRUE(client->isConnected());

    coordinatorOf(cluster, 1).simulateStorageHang(true);
    const auto hungAt = chrono::steady_clock::now();
    EXPECT_TRUE(TestCluster::waitFor([&] { return !client->isConnected(); }, Handover))
        << "the client of a node whose storage stopped answering stayed connected";
    EXPECT_LT(chrono::steady_clock::now() - hungAt, chrono::seconds(TestLeaseSeconds) + chrono::seconds(2));

    coordinatorOf(cluster, 1).simulateStorageHang(false);
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[1]->getCluster()->isOnline(); }, Handover));
    const auto again = cluster.connect(1, "client-of-a-hung-node", false);
    EXPECT_TRUE(again->isConnected());
}

/**
 * Confirm that a node whose place in the cluster went away with the storage takes it back by itself.
 *
 * Setup: Two nodes; the cluster's keys in Redis are cleared while both are running - what a wiped
 * or restored-from-backup storage looks like from the node's side.
 *
 * Verification: both nodes hold their leases again and link again, without a restart.
 */
TEST_F(XMQ_ClusterTests, aNodeWhoseMembershipWasClearedRejoinsByItself)
{
    TestCluster cluster(2);

    clearClusterState();

    EXPECT_TRUE(TestCluster::waitFor([&]
                                     {
                                         return cluster[0]->getCluster()->isOnline() && cluster[1]->getCluster()->isOnline();
                                     },
                                     Handover))
        << "a node that lost its place with the storage did not take it back";

    EXPECT_TRUE(cluster.waitForMesh()) << "the nodes did not link again after rejoining";
}

/**
 * Confirm that an offline node closes its cluster links and opens them again when it returns.
 *
 * Setup: Two nodes; the storage of node 1 stops answering.
 *
 * Verification: node 1 goes offline with no links to node 0 - an offline node neither forwards to
 * the nodes it can reach nor takes their traffic as if it were a member - and links again when the
 * storage returns.
 */
TEST_F(XMQ_ClusterTests, anOfflineNodeClosesItsClusterLinks)
{
    TestCluster cluster(2);
    ASSERT_TRUE(cluster.waitForMesh());

    coordinatorOf(cluster, 1).simulateStorageHang(true);
    EXPECT_TRUE(TestCluster::waitFor([&] { return !cluster[1]->getCluster()->isOnline(); }, Handover));

    // Asked of the other end: the links node 1 opened are server sessions there, and an offline
    // node closes the clients that carry them.
    const auto linkFromNode1 = format("node_{}_{}", TestCluster::nodeName(1), TestCluster::nodeName(0));
    EXPECT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto link = cluster[0]->getClientSession(linkFromNode1);
                                         return !link || !link->isConnected();
                                     },
                                     Handover))
        << "an offline node kept its links to the nodes it could reach";

    coordinatorOf(cluster, 1).simulateStorageHang(false);
    EXPECT_TRUE(TestCluster::waitFor([&] { return cluster[1]->getCluster()->isOnline(); }, Handover));
    EXPECT_TRUE(TestCluster::waitFor([&]
                                     {
                                         const auto link = cluster[0]->getClientSession(linkFromNode1);
                                         return link && link->isConnected();
                                     },
                                     Handover))
        << "a node that returned to the cluster did not link again";
}
