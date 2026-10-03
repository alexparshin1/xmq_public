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

#include "TestOptions.h"
#include "test/ClusterTests/ClusterTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Test that the message left for a subscriber on the origin cluster node is loaded upon the connection to another node.
 */
TEST_F(XMQ_ClusterTests, loadSession)
{
    {
        auto [primaryNode, secondaryNode] = makeClusterOfTwoNodes();

        COUT("──────────────────────────[Cluster nodes started]───────────────────────────────────────────");

        auto [publisher, subscriber, topicName] =
            createTestSubscriberAndPublisher(m_primaryServerHost, m_primaryServerHost);

        ASSERT_TRUE(publisher->isConnected());
        ASSERT_TRUE(subscriber->isConnected());

        Semaphore receivedMessage;
        subscriber->onMessage([&receivedMessage](const SPublishMessage&)
                              {
                                  receivedMessage.post();
                              });

        subscriber->disconnect();
        this_thread::sleep_for(100ms);

        ConnectCredentials publisherCredentials(publisher->getClientId(), "user", "secret");
        ConnectCredentials subscriberCredentials(subscriber->getClientId(), "user", "secret");

        COUT("──────────────────────────[Subscriber disconnected from primary]────────────────────────────");

        publisher->publish(topicName, "message");
        if (receivedMessage.wait_for(100ms))
        {
            FAIL() << "Not expected message received";
        }

        COUT("──────────────────────────[Subscriber re-connecting to secondary]───────────────────────────");

        // Re-connect the subscriber to the secondary node, expecting the received message:
        client::ConnectParameters connectParameters {.m_cleanSession = false};
        subscriber->connect(m_secondaryServerHost, subscriberCredentials, connectParameters);
        ASSERT_TRUE(subscriber->isConnected());
        this_thread::sleep_for(500ms);

        COUT("──────────────────────────[Subscriber re-connected]─────────────────────────────────────────");

        // Expect the message to be received after reconnection.
        if (!receivedMessage.wait_for(200ms))
        {
            FAIL() << "Expected message not received";
        }

        // Expect the subscription to continue working after reconnection.
        publisher->connect(m_secondaryServerHost, publisherCredentials, connectParameters);
        publisher->publish(topicName, "message 2");
        if (!receivedMessage.wait_for(100ms))
        {
            FAIL() << "Subscription is not restored after reconnection";
        }
    }
}
