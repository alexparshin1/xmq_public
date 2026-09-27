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
 * Test that cluster nodes know about each other after the startup.
 */
TEST_F(XMQ_ClusterTests, attachAndDetachToCluster)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    const auto   primaryNode = createNode("primary", 1880, true);
    const auto   secondaryNode = createNode("secondary", 1886, false);
    const auto   thirdNode = createNode("third", 1881, false);

    EXPECT_FALSE(primaryNode->cluster()->hasNodes());

    secondaryNode->attachToCluster(Host("localhost", 1880));
    this_thread::sleep_for(100ms);

    EXPECT_TRUE(primaryNode->cluster()->hasNodes());
    EXPECT_TRUE(secondaryNode->cluster()->hasNodes());
    EXPECT_EQ(1U, primaryNode->cluster()->getConnectedNodeCount("^(secondary|third)$"));
    EXPECT_EQ(1U, secondaryNode->cluster()->getConnectedNodeCount("^(primary|third)$"));

    thirdNode->attachToCluster(Host("localhost", 1880));
    this_thread::sleep_for(100ms);

    // Verify that nodes can see each others:
    EXPECT_EQ(2U, primaryNode->cluster()->getConnectedNodeCount("^(secondary|third)$"));
    EXPECT_EQ(2U, secondaryNode->cluster()->getConnectedNodeCount("^(primary|third)$"));
    EXPECT_EQ(2U, thirdNode->cluster()->getConnectedNodeCount("^(primary|secondary)$"));

    secondaryNode->detachFromCluster();
    thirdNode->detachFromCluster();
    this_thread::sleep_for(100ms);

    // Verify that there are no cluster nodes left:
    EXPECT_EQ(0U, primaryNode->cluster()->getClusterNodeCount());
    EXPECT_EQ(0U, secondaryNode->cluster()->getClusterNodeCount());
    EXPECT_EQ(0U, thirdNode->cluster()->getClusterNodeCount());
    EXPECT_FALSE(primaryNode->cluster()->hasNodes());
    EXPECT_FALSE(secondaryNode->cluster()->hasNodes());
}

/**
 * Test that a client connected to a node within the cluster is disconnected when another client with the
 * same client ID is connected to another node.
 */
TEST_F(XMQ_ClusterTests, enforceUniqueClientId)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    const auto   primaryNode = createNode("primary", 1880, true);
    const auto   secondaryNode = createNode("secondary", 1886, false);

    secondaryNode->attachToCluster(Host("localhost", 1880));
    this_thread::sleep_for(300ms);

    ConnectCredentials credentials("test-client", "user", "secret");
    client::MqttClient clientOnPrimary(logEngine());
    clientOnPrimary.connect(m_primaryServerHost, credentials, {});
    ASSERT_TRUE(clientOnPrimary.isConnected());

    // Let the primary's announcement reach the secondary before the second client arrives.
    // Connecting them at the same instant is deliberately not a defined case: each node tells the
    // others to drop the id as it accepts it, with nothing in the message to say which connection
    // is the newer, so two simultaneous arrivals on different nodes disconnect each other. One
    // client is expected to use one node, and the same id appearing on two at once is a fault in
    // its own right. What this test is for is the takeover, so it stages only that.
    this_thread::sleep_for(300ms);

    client::MqttClient clientOnSecondary(logEngine());
    clientOnSecondary.connect(m_secondaryServerHost, credentials, {});
    ASSERT_TRUE(clientOnSecondary.isConnected()) << "The second client did not connect at all";

    this_thread::sleep_for(300ms);
    ASSERT_FALSE(clientOnPrimary.isConnected());
    ASSERT_TRUE(clientOnSecondary.isConnected());

    clientOnPrimary.disconnect();
    clientOnSecondary.disconnect();
}

/**
 * Test that a client connected to a node within the cluster is disconnected when another client with the
 * same client ID is connected to another node.
 */
TEST_F(XMQ_ClusterTests, bridgeMessages)
{
    const string databaseUri = "postgresql://gtest@localhost/xmq_test";
    const auto   primaryNode = createNode("primary", 1880, true);
    const auto   secondaryNode = createNode("secondary", 1886, false);

    debugLog(true);

    client::MqttClient clientOnPrimary(logEngine());
    clientOnPrimary.connect(m_primaryServerHost,
                            ConnectCredentials{"test-client-1880", "user", "secret"}, {});
    ASSERT_TRUE(clientOnPrimary.isConnected());

    client::MqttClient clientOnSecondary(logEngine());
    clientOnSecondary.connect(m_secondaryServerHost,
                              ConnectCredentials{"test-client-1886", "user", "secret"}, {});
    clientOnSecondary.subscribe("topic/1");
    clientOnSecondary.onMessage([](const SPublishMessage& message)
    {
        COUT_TS("Client on Secondary: Received message: " << message->toString());
    });
    ASSERT_TRUE(clientOnSecondary.isConnected());

    this_thread::sleep_for(100ms);

    for (size_t i = 0; i < 1; ++i)
    {
        clientOnPrimary.publish("topic/1", "Test Message");
    }

    this_thread::sleep_for(10ms);

    clientOnPrimary.disconnect();
    clientOnSecondary.disconnect();
}

/**
 * Test that a client connected to a node within the cluster is disconnected when another client with the
 * same client ID is connected to another node.
 */
TEST_F(XMQ_ClusterTests, persistentConnectionPerformance)
{
    try
    {
        constexpr auto concurrentClients = 1000;
        constexpr auto threadCount = 10;
        const string   databaseUri = "postgresql://gtest@localhost/xmq_test";
        const auto     primaryNode = createNode("primary", 1880, true);
        this_thread::sleep_for(100ms);

        auto logger = logEngine();
        logger->minPriority(LogPriority::Error);

        vector<client::SMqttClient>            clientVector;
        SynchronizedQueue<client::SMqttClient> clients;
        for (size_t i = 0; i < concurrentClients; ++i)
        {
            auto client = make_shared<client::MqttClient>(logger);
            clients.push_back(client);
            clientVector.push_back(client);
        }

        Stopwatch watch;
        watch.start();

        atomic_size_t connectedClients = 0;
        auto          threadNumber = 0;

        vector<future<void>> futures;
        for (auto i = 0; i < threadCount; ++i)
        {
            auto threadFunction = [&clients, &connectedClients, threadNumber]
            {
                auto                i = 0;
                client::SMqttClient client;
                while (clients.pop_front(client, 1ms))
                {
                    ConnectCredentials credentials(format("test-client-{}-{}", threadNumber, i), "user", "secret");
                    client->connect(m_primaryServerHost, credentials, {});
                    ASSERT_TRUE(client->isConnected());
                    ++i;
                    ++connectedClients;
                }
            };

            futures.push_back(async(launch::async, threadFunction));

            threadNumber++;
        }

        for (const auto& future: futures)
        {
            if (future.wait_for(30s) != std::future_status::ready)
            {
                FAIL() << "Client connection timeout";
            }
        }

        watch.stop();
        COUT(std::format("Connected {:d} clients, {:0.2f} K/sec", connectedClients.load(), concurrentClients / watch.milliseconds()));

        for (const auto& client: clientVector)
        {
            client->disconnect();
        }
        clientVector.clear();
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}