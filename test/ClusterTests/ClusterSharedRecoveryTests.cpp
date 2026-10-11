/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"

#include <mutex>
#include <set>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The payloads every member of a group received between them, each counted once.
class Payloads
{
public:
    /// Take what a client receives, optionally after keeping it waiting - a slow consumer, whose
    /// deliveries are in flight while it works.
    void listen(const client::SMqttClient& client, const chrono::milliseconds work = 0ms)
    {
        client->onMessage([this, work](const SPublishMessage& message)
                          {
                              if (work > 0ms)
                              {
                                  this_thread::sleep_for(work);
                              }
                              const scoped_lock lock(m_mutex);
                              m_payloads.emplace(message->payload());
                              ++m_deliveries;
                          });
    }

    [[nodiscard]] size_t count() const
    {
        const scoped_lock lock(m_mutex);
        return m_payloads.size();
    }

    /// Every delivery, repeats included.
    [[nodiscard]] size_t deliveries() const
    {
        const scoped_lock lock(m_mutex);
        return m_deliveries;
    }

private:
    mutable mutex m_mutex;
    set<string>   m_payloads;
    size_t        m_deliveries {0};
};

void subscribeQos1(const client::SMqttClient& client, const string& filter)
{
    client->subscribe(Destination(client::MqttClient::getTopic(filter), SubscriptionOptions(Qos::Qos1)));
}

/// Publish count messages, numbered, a little apart.
void publishNumbered(const client::SMqttClient& publisher, const string& topic, const int count)
{
    for (int i = 0; i < count; ++i)
    {
        publisher->publish(topic, format("message {}", i), Qos::Qos1);
        this_thread::sleep_for(1ms);
    }
}

} // namespace

/**
 * Confirm that no QoS 1 message of a shared subscription is lost when the node of one of its members
 * stops while messages are flowing.
 *
 * Setup: Two nodes, one member of a shared subscription on each, subscribed at QoS 1. A stream of
 * QoS 1 messages is published on node 0; half way through, node 1 stops.
 *
 * Verification: Every message reaches a member. QoS 1 allows a message to come twice, so each
 * payload is counted once; it does not allow one to be lost - a delivery chosen for node 1 that
 * did not get there, or that node 1's member did not acknowledge, has to go to the member left.
 */
TEST_F(XMQ_ClusterTests, sharedQos1MessagesSurviveAMemberNodeStopping)
{
    TestCluster  cluster(2);
    const string topic = "recovery/node-stops";
    const string filter = "$share/recovery/" + topic;

    Payloads   received;
    const auto member0 = cluster.connect(0, "recovery-member-0");
    const auto member1 = cluster.connect(1, "recovery-member-1");
    received.listen(member0);
    received.listen(member1, 2ms);
    subscribeQos1(member0, filter);
    subscribeQos1(member1, filter);
    ASSERT_TRUE(TestCluster::waitFor([&]
                                     {
                                         return cluster[0]->getCluster()->getNodeSubscriptions(TestCluster::nodeName(1)).contains(filter);
                                     }));

    constexpr int count = 400;
    const auto    publisher = cluster.connect(0, "recovery-publisher");
    thread        publishing([&publisher, &topic] { publishNumbered(publisher, topic, count); });
    this_thread::sleep_for(200ms);
    cluster.stopNode(1);
    publishing.join();

    EXPECT_TRUE(TestCluster::waitFor([&received] { return received.count() == count; }, 5s))
        << count - static_cast<int>(received.count()) << " of " << count << " messages were lost";
}

/**
 * Confirm that the QoS 1 messages a shared-subscription member had not acknowledged go to another
 * member when its session ends - MQTT 5 section 4.8.2 says they should.
 *
 * Setup: One node; two members of a shared subscription with clean sessions, at QoS 1, one of them
 * slow. A stream of QoS 1 messages is published; part way through, the slow member's connection
 * breaks - hangup(), no DISCONNECT - which ends its session with deliveries in flight.
 *
 * Verification: Every message reaches a member, each counted once.
 */
TEST_F(XMQ_ClusterTests, sharedQos1MessagesOfAnEndedSessionGoToAnotherMember)
{
    TestCluster  cluster(1);
    const string topic = "recovery/session-ends";
    const string filter = "$share/recovery/" + topic;

    Payloads   received;
    const auto steady = cluster.connect(0, "recovery-steady");
    const auto slow = cluster.connect(0, "recovery-slow");
    received.listen(steady);
    received.listen(slow, 20ms);
    subscribeQos1(steady, filter);
    subscribeQos1(slow, filter);

    constexpr int count = 200;
    const auto    publisher = cluster.connect(0, "recovery-publisher");
    thread        publishing([&publisher, &topic] { publishNumbered(publisher, topic, count); });
    this_thread::sleep_for(100ms);
    slow->hangup();
    publishing.join();

    EXPECT_TRUE(TestCluster::waitFor([&received] { return received.count() == count; }, 5s))
        << count - static_cast<int>(received.count()) << " of " << count << " messages were lost";
}

/**
 * Confirm that a message published the moment a session has moved reaches it - before the node it
 * left has heard where its subscriptions are now.
 *
 * Setup: Two nodes. A persistent subscriber on node 0 connects again, under the same client id, to
 * node 1; a message is published on node 0 as soon as the second connection is accepted.
 *
 * Verification: The subscriber gets it.
 */
TEST_F(XMQ_ClusterTests, messagePublishedAsASessionMovesReachesIt)
{
    const TestCluster cluster(2);
    const string      topic = "recovery/moving";
    const string      clientId = "recovery-moving-subscriber";

    const auto first = cluster.connect(0, clientId, false);
    subscribeQos1(first, topic);
    const auto publisher = cluster.connect(0, "recovery-moving-publisher");

    Payloads   received;
    const auto second = make_shared<client::MqttClient>(logEngine());
    received.listen(second);
    ASSERT_EQ(ReasonCode::Success, second->connect(TestCluster::host(1), ConnectCredentials(clientId, "user", "secret"),
                                                   {.m_cleanSession = false}, ProtocolVersion::MqttV5));
    publisher->publish(topic, "as it moves", Qos::Qos1);

    EXPECT_TRUE(TestCluster::waitFor([&received] { return received.count() == 1; }, 3s))
        << "the message published as the session moved was lost";
    this_thread::sleep_for(300ms);
    EXPECT_EQ(1U, received.deliveries()) << "the message came twice";
}

/**
 * Confirm that a stream of messages published while a session moves reaches it whole, each message
 * once.
 *
 * Setup: Two nodes. A persistent subscriber on node 0; a stream of QoS 1 messages is published on
 * node 0 while the subscriber connects again, under the same client id, to node 1.
 *
 * Verification: Every message arrives - those queued on node 0 before the move, those sent on by
 * name during it, those routed to node 1 after it - and none twice.
 */
TEST_F(XMQ_ClusterTests, messagesPublishedWhileASessionMovesArriveOnce)
{
    const TestCluster cluster(2);
    const string      topic = "recovery/moving-stream";
    const string      clientId = "recovery-moving-stream-subscriber";

    auto first = cluster.connect(0, clientId, false);
    subscribeQos1(first, topic);
    first->disconnect();
    const auto publisher = cluster.connect(0, "recovery-moving-stream-publisher");

    constexpr int count = 200;
    thread        publishing([&publisher, &topic] { publishNumbered(publisher, topic, count); });
    this_thread::sleep_for(80ms);

    Payloads   received;
    const auto second = make_shared<client::MqttClient>(logEngine());
    received.listen(second);
    const auto connected = second->connect(TestCluster::host(1), ConnectCredentials(clientId, "user", "secret"),
                                           {.m_cleanSession = false}, ProtocolVersion::MqttV5);
    publishing.join();
    ASSERT_EQ(ReasonCode::Success, connected) << "node 0 did not let the session go";

    EXPECT_TRUE(TestCluster::waitFor([&received] { return received.count() == count; }, 5s))
        << count - static_cast<int>(received.count()) << " of " << count << " messages were lost";
    this_thread::sleep_for(300ms);
    EXPECT_EQ(static_cast<size_t>(count), received.deliveries()) << "messages came twice";
}
