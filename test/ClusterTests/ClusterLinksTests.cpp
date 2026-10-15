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

#include <map>
#include <mutex>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// What a client received, topic by topic, in the order it arrived.
class ByTopic
{
public:
    void listen(const client::SMqttClient& client)
    {
        client->onMessage([this](const SPublishMessage& message)
                          {
                              const scoped_lock lock(m_mutex);
                              m_received[string(message->destination()->fullName())].push_back(string(message->payload()));
                              ++m_total;
                          });
    }

    [[nodiscard]] size_t total() const
    {
        const scoped_lock lock(m_mutex);
        return m_total;
    }

    [[nodiscard]] vector<string> topic(const string& name) const
    {
        const scoped_lock lock(m_mutex);
        const auto        found = m_received.find(name);
        return found == m_received.end() ? vector<string>() : found->second;
    }

private:
    mutable mutex                m_mutex;
    map<string, vector<string>> m_received;
    size_t                       m_total {0};
};

void subscribeQos1(const client::SMqttClient& client, const string& filter)
{
    client->subscribe(Destination(client::MqttClient::getTopic(filter), SubscriptionOptions(Qos::Qos1)));
}

constexpr size_t LinkCount = 4;
constexpr int    Topics = 40;
constexpr int    PerTopic = 5;

} // namespace

/**
 * Scenario: A node has several links to each other node.
 *
 * Verification: Every one of them is a session on the other node, and knows which one it is.
 */
TEST_F(XMQ_ClusterTests, aNodeHasSeveralLinksToEachOtherNode)
{
    const TestCluster cluster(2);

    for (size_t link = 0; link < LinkCount; ++link)
    {
        const auto id = "node_" + TestCluster::nodeName(1) + "_" + TestCluster::nodeName(0) + (link > 0 ? "#" + to_string(link) : "");
        ASSERT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getClientSession(id) != nullptr; })) << id;
        const auto session = cluster[0]->getClientSession(id);
        EXPECT_EQ(LinkCount, session->clusterLinkCount()) << id;
        EXPECT_EQ(link, session->clusterLinkIndex()) << id;
    }
}

/**
 * Scenario: Many topics, exact and wildcard, are published on one node and subscribed to on another,
 * which has several links to it. Another subscriber's filter covers some of the exact ones.
 *
 * Verification: Each subscriber receives each message once, and each topic's messages in the order
 * they were published - whichever link carried them.
 */
TEST_F(XMQ_ClusterTests, topicsOverSeveralLinksArriveOnceAndInOrder)
{
    const TestCluster cluster(2);

    ByTopic    exact;
    ByTopic    wildcard;
    const auto exactSubscriber = cluster.connect(1, "links-exact-subscriber");
    const auto wildcardSubscriber = cluster.connect(1, "links-wildcard-subscriber");
    exact.listen(exactSubscriber);
    wildcard.listen(wildcardSubscriber);
    for (int topic = 0; topic < Topics; ++topic)
    {
        subscribeQos1(exactSubscriber, format("links/exact/{}", topic));
    }
    subscribeQos1(wildcardSubscriber, "links/#");
    ASSERT_TRUE(TestCluster::waitFor([&] { return cluster[0]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(1)).size() >= Topics + 1; }));

    const auto publisher = cluster.connect(0, "links-publisher");
    for (int message = 0; message < PerTopic; ++message)
    {
        for (int topic = 0; topic < Topics; ++topic)
        {
            publisher->publish(format("links/exact/{}", topic), format("message {}", message), Qos::Qos1);
            publisher->publish(format("links/other/{}", topic), format("message {}", message), Qos::Qos1);
        }
    }

    ASSERT_TRUE(TestCluster::waitFor([&] { return exact.total() >= Topics * PerTopic && wildcard.total() >= 2 * Topics * PerTopic; },
                                     chrono::seconds(15)));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(static_cast<size_t>(Topics * PerTopic), exact.total());
    EXPECT_EQ(static_cast<size_t>(2 * Topics * PerTopic), wildcard.total());

    vector<string> expected;
    for (int message = 0; message < PerTopic; ++message)
    {
        expected.push_back(format("message {}", message));
    }
    for (int topic = 0; topic < Topics; ++topic)
    {
        EXPECT_EQ(expected, exact.topic(format("links/exact/{}", topic))) << topic;
        EXPECT_EQ(expected, wildcard.topic(format("links/exact/{}", topic))) << topic;
        EXPECT_EQ(expected, wildcard.topic(format("links/other/{}", topic))) << topic;
    }
}

