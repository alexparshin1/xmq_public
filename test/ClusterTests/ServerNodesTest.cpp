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

#include "../../server/Cluster/ServerNodes.h"
#include "../../server/Settings/Settings.h"
#include "ClusterTests.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;
using namespace xmq::cluster;

namespace {
}

TEST_F(XMQ_ClusterTests, AddServerNode)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    EXPECT_NO_THROW(serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate));
    EXPECT_EQ(serverNodes.size(), 1U);
}

TEST_F(XMQ_ClusterTests, RemoveNode)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate);
    EXPECT_EQ(serverNodes.size(), 1U);

    serverNodes.setNodeToOffline(node);
    EXPECT_EQ(serverNodes.size(), 0U);
}

TEST_F(XMQ_ClusterTests, FindNodeByName)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate);

    auto foundNode = serverNodes.findNode("TestNode1");
    ASSERT_NE(foundNode, nullptr);
    EXPECT_EQ(foundNode->getNodeSettings().m_node_name.asString(), "TestNode1");

    auto notFoundNode = serverNodes.findNode("NonExistentNode");
    EXPECT_EQ(notFoundNode, nullptr);
}

TEST_F(XMQ_ClusterTests, ClearAllNodes)
{
    const auto primaryNode = createNode("primary", 1880, true);

    Topics      topics(server()->getTopicManager());
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    CServerNode nodeSettings1;
    nodeSettings1.m_node_name = "TestNode1";
    const auto node1 = std::make_shared<ServerNode>(server().get(), nodeSettings1, topics);

    CServerNode nodeSettings2;
    nodeSettings2.m_node_name = "TestNode2";
    const auto node2 = std::make_shared<ServerNode>(server().get(), nodeSettings2, topics);

    serverNodes.addNode(node1, StoreNodeMode::InsertOrUpdate);
    serverNodes.addNode(node2, StoreNodeMode::InsertOrUpdate);

    EXPECT_EQ(serverNodes.size(), 2U);
    serverNodes.clear();
    EXPECT_EQ(serverNodes.size(), 0U);
}

TEST_F(XMQ_ClusterTests, GetNodeNames)
{
    const auto primaryNode = createNode("primary", 1880, true);

    Topics      topics(server()->getTopicManager());
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    CServerNode nodeSettings1;
    nodeSettings1.m_node_name = "TestNode1";
    const auto node1 = std::make_shared<ServerNode>(server().get(), nodeSettings1, topics);

    CServerNode nodeSettings2;
    nodeSettings2.m_node_name = "TestNode2";
    const auto node2 = std::make_shared<ServerNode>(server().get(), nodeSettings2, topics);

    serverNodes.addNode(node1, StoreNodeMode::InsertOrUpdate);
    serverNodes.addNode(node2, StoreNodeMode::InsertOrUpdate);

    auto names = serverNodes.getNodeNames();
    EXPECT_EQ(names.size(), 2U);
    EXPECT_TRUE(ranges::find(names, "TestNode1") != names.end());
    EXPECT_TRUE(ranges::find(names, "TestNode2") != names.end());
}

TEST_F(XMQ_ClusterTests, IsEmptyMethod)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());
    EXPECT_TRUE(serverNodes.empty());

    serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate);
    EXPECT_FALSE(serverNodes.empty());

    serverNodes.clear();
    EXPECT_TRUE(serverNodes.empty());
}

TEST_F(XMQ_ClusterTests, AddDuplicateNode)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());
    EXPECT_TRUE(serverNodes.empty());

    serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate);
    EXPECT_EQ(serverNodes.size(), 1U);

    serverNodes.addNode(node, StoreNodeMode::InsertOrUpdate);
    EXPECT_EQ(serverNodes.size(), 1U);
}

TEST_F(XMQ_ClusterTests, RemoveNonExistentNode)
{
    const auto primaryNode = createNode("primary", 1880, true);

    CServerNode nodeSettings;
    nodeSettings.m_node_name = "TestNode1";

    Topics      topics(server()->getTopicManager());
    const auto  node = std::make_shared<ServerNode>(server().get(), nodeSettings, topics);
    ServerNodes serverNodes(server()->getCluster().get(), server()->getStorage());

    EXPECT_NO_THROW(serverNodes.setNodeToOffline(node));
    EXPECT_EQ(serverNodes.size(), 0U);
}