/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"
#include "TestOptions.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Test that the message is passed from the origin cluster node to another.
 */
TEST_F(XMQ_ClusterTests, passMessages_twoNodes)
{
    auto [primaryNode, secondaryNode] = makeClusterOfTwoNodes();

    COUT("──────────────────────────[Cluster nodes started]───────────────────────────────────────────");

    auto [publisher, subscriber, topicName] =
        createTestSubscriberAndPublisher(m_primaryServerHost, m_primaryServerHost);

    ASSERT_TRUE(publisher->isConnected());
    subscriber->disconnect();

    Semaphore receivedMessage;
    subscriber->onMessage([&receivedMessage](const SPublishMessage&)
                          {
                              receivedMessage.post();
                          });

    this_thread::sleep_for(100ms);

    COUT("──────────────────────────[Subscriber re-connecting to secondary]───────────────────────────");

    ConnectCredentials        subscriberCredentials(subscriber->getClientId(), "user", "secret");
    client::ConnectParameters connectParameters {.m_cleanSession = false};
    subscriber->connect(m_secondaryServerHost, subscriberCredentials, connectParameters);
    ASSERT_TRUE(subscriber->isConnected());
    this_thread::sleep_for(500ms);

    COUT("──────────────────────────[Subscriber re-connected]─────────────────────────────────────────");

    publisher->publish(topicName, "message 2");
    if (!receivedMessage.wait_for(1s))
    {
        FAIL() << "Message from primary not received on secondary";
    }
}

/**
 * Test that the message is passed from the origin cluster node to another.
 */
TEST_F(XMQ_ClusterTests, passMessages_multipleNodes)
{
    constexpr auto    nodeCount = 4u;
    const TestCluster cluster(nodeCount);

    COUT("──────────────────────────[Cluster nodes started]───────────────────────────────────────────");

    ConnectCredentials        credentials("cluster_publisher", "user", "secret");
    client::ConnectParameters connectParameters {.m_cleanSession = true};

    client::MqttClient publisher(logEngine());
    publisher.connect(TestCluster::host(0), credentials, connectParameters);
    ASSERT_TRUE(publisher.isConnected());

    vector<client::SMqttClient> subscribers;

    mutex               receivedMessageMutex;
    map<string, size_t> receivedMessageCounts;
    Semaphore           receivedAllMessages;

    connectParameters.m_cleanSession = false;
    for (auto nodeIndex = 0u; nodeIndex < nodeCount; ++nodeIndex)
    {
        const auto         clientId = format("cluster_subscriber_{}", nodeIndex);
        ConnectCredentials credentials2(clientId, "user", "secret");

        auto subscriber = make_shared<client::MqttClient>();
        subscriber->connect(TestCluster::host(nodeIndex), credentials2, connectParameters);
        ASSERT_TRUE(subscriber->isConnected());

        subscriber->onMessage([clientId, &receivedMessageMutex, &receivedMessageCounts, &receivedAllMessages](const SPublishMessage& message)
                              {
                                  scoped_lock lock(receivedMessageMutex);
                                  receivedMessageCounts[clientId]++;
                                  if (receivedMessageCounts.size() == nodeCount) receivedAllMessages.post();
                                  COUT("Received message from " << clientId << ": " << message->toString());
                              });

        subscriber->subscribe(Destination(client::MqttClient::getTopic("topic1")));
        subscribers.push_back(subscriber);
    }

    // A node is sent only what its clients subscribe to, so the publishing node has to have heard of
    // every remote subscription first - which a subscribe takes a moment to reach, as in any broker.
    for (auto nodeIndex = 1u; nodeIndex < nodeCount; ++nodeIndex)
    {
        ASSERT_TRUE(TestCluster::waitFor([&cluster, nodeIndex]
                                         {
                                             return cluster[0]->getCluster()
                                                 ->getNodeSubscriptions(TestCluster::nodeName(nodeIndex))
                                                 .contains("topic1");
                                         }))
            << "node 0 did not learn of the subscription on " << TestCluster::nodeName(nodeIndex);
    }

    COUT("──────────────────────────[Publisher and subscribers started]───────────────────────────────");

    Stopwatch stopwatch;
    publisher.publish("topic1", "message");

    const auto done = receivedAllMessages.wait_for(1s);

    stopwatch.stop();

    if (done)
    {
        COUT("Received all messages for " << fixed << setprecision(2) << stopwatch.milliseconds() << " ms.");
    }

    for (const auto& subscriber: subscribers)
    {
        if (auto count = receivedMessageCounts[subscriber->getClientId()]; count != 1)
        {
            FAIL() << "Node " << subscriber->getClientId() << " received " << count << " messages where only one is expected";
        }
    }
}
