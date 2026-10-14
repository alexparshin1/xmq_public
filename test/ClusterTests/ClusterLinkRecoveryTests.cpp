/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"

#include <atomic>

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Scenario: The link one node opened to another breaks while both stay up - the other node closed
 * it, or it timed out answering its CONNECT, as a node busy with clients' TLS handshakes does when
 * one of them comes back.
 *
 * Verification: The node opens the link again, and a message published on the other node reaches
 * a subscriber of this one. It used to stay unlinked from that node until it restarted: a link
 * was only ever opened when a node started or was attached.
 */
TEST_F(XMQ_ClusterTests, aBrokenLinkBetweenRunningNodesIsOpenedAgain)
{
    const TestCluster cluster(3);
    const string      topic = "links/recovered";

    atomic_int received {0};
    const auto subscriber = cluster.connect(2, "link-recovery-subscriber");
    subscriber->onMessage([&received](const SPublishMessage&) { ++received; });
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topic), SubscriptionOptions(Qos::Qos1)));
    ASSERT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(2)).contains(topic); }));

    // Node 2's links to node 0, closed by node 0: every one of them, as a timed-out CONNECT is.
    for (size_t link = 0; link < 4; ++link)
    {
        cluster[0]->closeSession("node_" + TestCluster::nodeName(2) + "_" + TestCluster::nodeName(0) + (link > 0 ? "#" + to_string(link) : ""));
    }

    const auto publisher = cluster.connect(0, "link-recovery-publisher");
    ASSERT_TRUE(TestCluster::waitFor(
        [&]
        {
            publisher->publish(topic, "after the break", Qos::Qos1);
            return received.load() > 0;
        },
        chrono::seconds(20)));
}
