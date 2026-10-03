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
#include "server/Cluster/ServerNode.h"
#include <sptk5/net/SSLSocket.h>
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

    secondaryNode->attachToCluster(primaryNode->getCluster()->getNodeHost());
    this_thread::sleep_for(100ms);

    EXPECT_TRUE(primaryNode->cluster()->hasNodes());
    EXPECT_TRUE(secondaryNode->cluster()->hasNodes());
    EXPECT_EQ(1U, primaryNode->cluster()->getConnectedNodeCount("^(secondary|third)$"));
    EXPECT_EQ(1U, secondaryNode->cluster()->getConnectedNodeCount("^(primary|third)$"));

    thirdNode->attachToCluster(primaryNode->getCluster()->getNodeHost());
    this_thread::sleep_for(100ms);

    // Verify that nodes can see each others:
    EXPECT_EQ(2U, primaryNode->cluster()->getConnectedNodeCount("^(secondary|third)$"));
    EXPECT_EQ(2U, secondaryNode->cluster()->getConnectedNodeCount("^(primary|third)$"));
    EXPECT_EQ(2U, thirdNode->cluster()->getConnectedNodeCount("^(primary|secondary)$"));

    // Inspect all six directed peer connections, including the links established by discovery.
    const vector nodes {primaryNode, secondaryNode, thirdNode};
    for (const auto& destination: nodes)
    {
        for (const auto& origin: nodes)
        {
            if (origin == destination)
            {
                continue;
            }
            const auto clientId = format("node_{}_{}", origin->getNodeName(), destination->getNodeName());
            const auto session = destination->getClientSession(clientId);
            ASSERT_NE(nullptr, session) << clientId;
            ASSERT_NE(nullptr, session->getConnection()) << clientId;
            EXPECT_NE(nullptr, dynamic_pointer_cast<SSLSocket>(session->getConnection()->getSocket())) << clientId;
        }
        for (const auto& peer: destination->cluster()->getClusterSettings().m_nodes)
        {
            EXPECT_TRUE(peer.m_encrypted.asBool());
        }
    }

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

    secondaryNode->attachToCluster(primaryNode->getCluster()->getNodeHost());
    this_thread::sleep_for(300ms);

    ConnectCredentials credentials("test-client", "user", "secret");
    client::MqttClient clientOnPrimary(logEngine());
    clientOnPrimary.connect(m_primaryServerHost, credentials, {});
    ASSERT_TRUE(clientOnPrimary.isConnected());

    // Let the primary's announcement reach the secondary before the second client arrives.
    // Concurrent connections may both be disconnected under the cluster requirements. This test
    // checks sequential takeover, so the first connection's announcement must reach its peers
    // before the second connection arrives.
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
 * Confirm that a caller cannot request an unencrypted cluster join.
 *
 * Setup: Start two nodes with both MQTT and MQTT+SSL listeners.
 * Verification: Request a plain join and expect rejection before either node registers a peer;
 * then join using the TLS endpoint and check that the same nodes can establish the cluster.
 */
TEST_F(XMQ_ClusterTests, unencryptedClusterJoinIsRejected)
{
    const auto primary = createNode("primary", 1880, true);
    const auto secondary = createNode("secondary", 1886, true);

    EXPECT_THROW(secondary->attachToCluster(m_primaryServerHost, false), Exception);
    EXPECT_FALSE(primary->cluster()->hasNodes());
    EXPECT_FALSE(secondary->cluster()->hasNodes());

    secondary->attachToCluster(primary->cluster()->getNodeHost());
    for (auto i = 0; i < 100 && (!primary->cluster()->hasNodes() || !secondary->cluster()->hasNodes()); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(primary->cluster()->hasNodes());
    EXPECT_TRUE(secondary->cluster()->hasNodes());
}

/**
 * Confirm that valid cluster credentials cannot establish a cluster session over plain MQTT.
 *
 * Setup: Start a node with plain and TLS listeners, and use identical cluster credentials on both.
 * Verification: The plain CONNECT is refused, while the TLS CONNECT succeeds. Inspect the accepted
 * session's socket to confirm encryption; a later plain CONNECT with its Client ID must neither
 * take over nor close that TLS session. A subsequent TLS CONNECT must take it over normally.
 * An ordinary client must be able to take over its TLS connection through the plain MQTT port.
 */
TEST_F(XMQ_ClusterTests, clusterCredentialsRequireTls)
{
    const auto primary = createNode("primary", 1880, true);
    const ConnectCredentials credentials("tls-cluster-peer", "cluster", "cluster");
    client::MqttClient plain;
    EXPECT_NE(ReasonCode::Success,
              plain.connect(m_primaryServerHost, credentials, {}, ProtocolVersion::MqttV5));
    EXPECT_FALSE(plain.isConnected());

    client::MqttClient encrypted;
    ASSERT_EQ(ReasonCode::Success,
              encrypted.connect(primary->cluster()->getNodeHost(), credentials, {},
                                ProtocolVersion::MqttV5, {}, make_shared<SSLKeys>()));
    const auto session = primary->getClientSession(credentials.getClientId());
    ASSERT_NE(nullptr, session);
    ASSERT_NE(nullptr, session->getConnection());
    EXPECT_NE(nullptr, dynamic_pointer_cast<SSLSocket>(session->getConnection()->getSocket()));

    for (const auto version: {ProtocolVersion::MqttV31, ProtocolVersion::MqttV311, ProtocolVersion::MqttV5})
    {
        EXPECT_NE(ReasonCode::Success, plain.connect(m_primaryServerHost, credentials, {}, version));
        EXPECT_TRUE(encrypted.isConnected());
    }

    client::MqttClient replacement;
    ASSERT_EQ(ReasonCode::Success,
              replacement.connect(primary->cluster()->getNodeHost(), credentials, {},
                                  ProtocolVersion::MqttV5, {}, make_shared<SSLKeys>()));
    for (auto i = 0; i < 100 && encrypted.isConnected(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(encrypted.isConnected());
    EXPECT_TRUE(replacement.isConnected());

    const ConnectCredentials ordinaryCredentials("ordinary-client", "user", "secret");
    client::MqttClient ordinaryTls;
    ASSERT_EQ(ReasonCode::Success,
              ordinaryTls.connect(primary->cluster()->getNodeHost(), ordinaryCredentials, {},
                                  ProtocolVersion::MqttV5, {}, make_shared<SSLKeys>()));
    client::MqttClient ordinary;
    ASSERT_EQ(ReasonCode::Success,
              ordinary.connect(m_primaryServerHost, ordinaryCredentials, {}, ProtocolVersion::MqttV5));
    for (auto i = 0; i < 100 && ordinaryTls.isConnected(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(ordinaryTls.isConnected());
    EXPECT_TRUE(ordinary.isConnected());
}

/**
 * Confirm that a discovered peer cannot downgrade the mesh to plain MQTT.
 *
 * Setup: Construct a peer record pointing to a node's plain listener with encryption disabled.
 * Verification: The low-level cluster connection rejects the record before opening a connection.
 */
TEST_F(XMQ_ClusterTests, unencryptedPeerRecordIsRejected)
{
    const auto primary = createNode("primary", 1880, true);
    CServerNode settings;
    settings.m_node_name = "plain-peer";
    settings.m_host_port = m_primaryServerHost.toString();
    settings.m_encrypted = false;
    cluster::ServerNode peer(primary.get(), settings, primary->cluster()->getClusterTopics());
    EXPECT_THROW(peer.connect(true), Exception);
    EXPECT_FALSE(peer.isConnected());
}

/**
 * Confirm that discovered peer certificate paths cannot replace the local TLS configuration.
 *
 * Setup: Start two nodes with valid local TLS keys. Construct a peer record whose key, certificate
 * and CA paths do not exist on this machine, and whose verification policy differs from the local one.
 * Verification: A cluster connection succeeds using the local node's keys and policy, and the
 * receiving broker reports an SSL socket for that session.
 */
TEST_F(XMQ_ClusterTests, peerTlsUsesLocalKeys)
{
    const auto primary = createNode("primary", 1880, true);
    const auto secondary = createNode("secondary", 1886, true);
    CServerNode settings;
    settings.m_node_name = primary->getNodeName();
    settings.m_host_port = primary->cluster()->getNodeHost().toString();
    settings.m_encrypted = true;
    settings.m_ssl_keys.m_keyfile = "missing-peer-key.pem";
    settings.m_ssl_keys.m_certfile = "missing-peer-certificate.pem";
    settings.m_ssl_keys.m_cafile = "missing-peer-ca.pem";
    settings.m_ssl_keys.m_verify_depth = 5;

    cluster::ServerNode peer(secondary.get(), settings, secondary->cluster()->getClusterTopics());
    ASSERT_EQ(ReasonCode::Success, peer.connect(true));
    const auto session = primary->getClientSession("node_secondary_primary");
    ASSERT_NE(nullptr, session);
    ASSERT_NE(nullptr, session->getConnection());
    EXPECT_NE(nullptr, dynamic_pointer_cast<SSLSocket>(session->getConnection()->getSocket()));
    peer.disconnect();
}
