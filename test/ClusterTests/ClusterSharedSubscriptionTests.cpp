/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "base/MessageProperties.h"
#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// A client counting what it receives, and keeping the last message.
struct Member
{
    client::SMqttClient client;
    atomic_int          received {0};
    mutex               lastMutex;
    SPublishMessage     last;
};

unique_ptr<Member> member(const TestCluster& cluster, const size_t node, const string& clientId, const string& filter)
{
    auto counting = make_unique<Member>();
    counting->client = cluster.connect(node, clientId);
    counting->client->onMessage(
        [m = counting.get()](const SPublishMessage& message)
        {
            {
                const scoped_lock lock(m->lastMutex);
                m->last = message;
            }
            ++m->received;
        });
    counting->client->subscribe(filter);
    return counting;
}

/// Wait until every other node has learned that this one has a subscriber for the filter.
bool knownEverywhere(const TestCluster& cluster, const size_t subscriberNode, const string& filter)
{
    return TestCluster::waitFor(
        [&]
        {
            for (size_t node = 0; node < cluster.size(); ++node)
            {
                if (node != subscriberNode &&
                    !cluster[node]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(subscriberNode)).contains(filter))
                {
                    return false;
                }
            }
            return true;
        });
}

int total(const vector<unique_ptr<Member>>& members)
{
    int sum = 0;
    for (const auto& m: members)
    {
        sum += m->received;
    }
    return sum;
}

string counts(const vector<unique_ptr<Member>>& members)
{
    string text;
    for (const auto& m: members)
    {
        text += (text.empty() ? "" : " ") + to_string(m->received.load());
    }
    return text;
}

/// Publish from a client on each node in turn; kept until the end, so nothing unsent is lost with it.
vector<client::SMqttClient> publishFromEveryNode(const TestCluster& cluster, const string& clientId, const string& topic,
                                                 const int perNode)
{
    vector<client::SMqttClient> publishers;
    for (size_t node = 0; node < cluster.size(); ++node)
    {
        const auto& publisher = publishers.emplace_back(cluster.connect(node, format("{}_{}", clientId, node)));
        for (auto i = 0; i < perNode; ++i)
        {
            publisher->publish(topic, format("message {} from node {}", i, node));
        }
    }
    return publishers;
}

} // namespace

/**
 * Confirm that a node sent a publication for another reason does not serve a shared subscription
 * the publication was not assigned to it for.
 *
 * Setup: Two nodes, a member of "$share/group/<topic>" on each, and a plain subscriber to <topic>
 * on node 1 - so node 1 is sent every publication. 20 messages are published on node 0.
 * Verification: The plain subscriber gets all 20; the shared subscription gets 20 in total,
 * where node 1 used to hand every one of them to its member as well.
 */
TEST_F(XMQ_ClusterTests, sharedSubscriptionIsNotServedAgainWhereAPlainSubscriberTookTheMessage)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();
    const auto     filter = "$share/group/" + topic;
    constexpr auto count = 20;

    vector<unique_ptr<Member>> members;
    members.push_back(member(cluster, 0, subscriberId + "_m0", filter));
    members.push_back(member(cluster, 1, subscriberId + "_m1", filter));
    const auto plain = member(cluster, 1, subscriberId + "_plain", topic);
    ASSERT_TRUE(knownEverywhere(cluster, 1, filter));
    ASSERT_TRUE(knownEverywhere(cluster, 1, topic));

    const auto publisher = cluster.connect(0, publisherId);
    for (auto i = 0; i < count; ++i)
    {
        publisher->publish(topic, format("message {}", i));
    }

    EXPECT_TRUE(TestCluster::waitFor([&] { return plain->received == count && total(members) >= count; }));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(count, plain->received);
    EXPECT_EQ(count, total(members)) << "members got " << counts(members);
}

/**
 * Confirm that independent shared subscriptions each get every publication once, whichever node it
 * enters through.
 *
 * Setup: Three nodes. "$share/a/<topic>" has one member on node 0 and two on node 1;
 * "$share/b/<topic>" has two on node 0 and one on node 2 - different counts, so the two groups do
 * not take turns in step. Each node publishes 15 messages.
 * Verification: Each group received the 45 messages between its members - none lost, none twice.
 */
TEST_F(XMQ_ClusterTests, independentSharedSubscriptionsEachGetEveryMessageOnce)
{
    const TestCluster cluster(3);
    const auto [publisherId, subscriberId, topic] = makeTestNames();
    const auto     filterA = "$share/a/" + topic;
    const auto     filterB = "$share/b/" + topic;
    constexpr auto perNode = 15;

    vector<unique_ptr<Member>> groupA;
    groupA.push_back(member(cluster, 0, subscriberId + "_a0", filterA));
    groupA.push_back(member(cluster, 1, subscriberId + "_a1", filterA));
    groupA.push_back(member(cluster, 1, subscriberId + "_a2", filterA));
    vector<unique_ptr<Member>> groupB;
    groupB.push_back(member(cluster, 0, subscriberId + "_b0", filterB));
    groupB.push_back(member(cluster, 0, subscriberId + "_b1", filterB));
    groupB.push_back(member(cluster, 2, subscriberId + "_b2", filterB));
    ASSERT_TRUE(knownEverywhere(cluster, 0, filterA));
    ASSERT_TRUE(knownEverywhere(cluster, 1, filterA));
    ASSERT_TRUE(knownEverywhere(cluster, 0, filterB));
    ASSERT_TRUE(knownEverywhere(cluster, 2, filterB));

    const auto expected = static_cast<int>(perNode * cluster.size());
    const auto publishers = publishFromEveryNode(cluster, publisherId, topic, perNode);

    EXPECT_TRUE(TestCluster::waitFor([&] { return total(groupA) >= expected && total(groupB) >= expected; }));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(expected, total(groupA)) << "group a got " << counts(groupA);
    EXPECT_EQ(expected, total(groupB)) << "group b got " << counts(groupB);
}

/**
 * Confirm that one share name with two filters is two shared subscriptions.
 *
 * Setup: Two nodes. "$share/group/<topic>/x" and "$share/group/<topic>/+" each have a member on
 * both nodes. Each node publishes 10 messages to "<topic>/x", which both filters match.
 * Verification: Each of the two subscriptions received the 20 messages once.
 */
TEST_F(XMQ_ClusterTests, oneShareNameWithTwoFiltersIsTwoSubscriptions)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();
    const auto     exact = "$share/group/" + topic + "/x";
    const auto     wildcard = "$share/group/" + topic + "/+";
    constexpr auto perNode = 10;

    vector<unique_ptr<Member>> exactMembers;
    exactMembers.push_back(member(cluster, 0, subscriberId + "_e0", exact));
    exactMembers.push_back(member(cluster, 1, subscriberId + "_e1", exact));
    vector<unique_ptr<Member>> wildcardMembers;
    wildcardMembers.push_back(member(cluster, 0, subscriberId + "_w0", wildcard));
    wildcardMembers.push_back(member(cluster, 1, subscriberId + "_w1", wildcard));
    for (size_t node = 0; node < cluster.size(); ++node)
    {
        ASSERT_TRUE(knownEverywhere(cluster, node, exact));
        ASSERT_TRUE(knownEverywhere(cluster, node, wildcard));
    }

    const auto expected = static_cast<int>(perNode * cluster.size());
    const auto publishers = publishFromEveryNode(cluster, publisherId, topic + "/x", perNode);

    EXPECT_TRUE(TestCluster::waitFor([&] { return total(exactMembers) >= expected && total(wildcardMembers) >= expected; }));
    this_thread::sleep_for(300ms);
    EXPECT_EQ(expected, total(exactMembers)) << "exact filter got " << counts(exactMembers);
    EXPECT_EQ(expected, total(wildcardMembers)) << "wildcard filter got " << counts(wildcardMembers);
}

/**
 * Confirm that the assignment a forwarded message carries between nodes never reaches a client,
 * and that the publisher's own user properties do.
 *
 * Setup: Two nodes; a member of "$share/group/<topic>" on node 1 only, so every publication on
 * node 0 is assigned to node 1. The publisher sets a user property of its own.
 * Verification: The member receives the publisher's property, and not the cluster's.
 */
TEST_F(XMQ_ClusterTests, clientsNeverSeeTheClusterAssignment)
{
    const TestCluster cluster(2);
    const auto [publisherId, subscriberId, topic] = makeTestNames();
    const auto filter = "$share/group/" + topic;

    const auto onlyMember = member(cluster, 1, subscriberId, filter);
    ASSERT_TRUE(knownEverywhere(cluster, 1, filter));

    const auto publisher = cluster.connect(0, publisherId);
    const auto properties = make_shared<MessageProperties>();
    properties->setUserProperty("origin", "test");
    publisher->publish(client::MqttClient::getTopic(topic), Buffer("assigned to node 1"), Qos::Qos1, properties);

    ASSERT_TRUE(TestCluster::waitFor([&] { return onlyMember->received == 1; }));
    const scoped_lock lock(onlyMember->lastMutex);
    const auto&       received = onlyMember->last->getProperties();
    ASSERT_TRUE(received);
    EXPECT_EQ("test", received->getUserProperty("origin"));
    EXPECT_TRUE(received->getUserProperty(PublishMessage::ClusterShareProperty).empty())
        << "The cluster's assignment reached a client";
}
