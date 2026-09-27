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

#include "ServerNodes.h"

#include "Cluster.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq::cluster;

ServerNodes::ServerNodes(Cluster* cluster, const SStorage& storage)
    : m_cluster(cluster)
    , m_storage(storage)
{
}

void ServerNodes::addNode(const SServerNode& node, const StoreNodeMode)
{
    const unique_lock lock(m_mutex);
    if (node)
    {
        storeNodeRecord(node);
        m_nodes[node->getName()] = node;
        m_count.store(m_nodes.size(), std::memory_order_relaxed);
    }
}

void ServerNodes::setNodeToOffline(const SServerNode& node)
{
    const unique_lock lock(m_mutex);
    if (node)
    {
        setNodeRecordToOffline(node);
        m_nodes.erase(node->getName());
        m_count.store(m_nodes.size(), std::memory_order_relaxed);
    }
}

void ServerNodes::clear()
{
    const unique_lock lock(m_mutex);
    m_nodes.clear();
    m_count.store(0, std::memory_order_relaxed);
}

bool ServerNodes::empty() const
{
    const shared_lock lock(m_mutex);
    return m_nodes.empty();
}

size_t ServerNodes::size() const
{
    const shared_lock lock(m_mutex);
    return m_nodes.size();
}

SServerNode ServerNodes::findNode(const std::string& nodeName) const
{
    const shared_lock lock(m_mutex);
    const auto        it = m_nodes.find(nodeName);
    if (it == m_nodes.end())
    {
        return nullptr;
    }
    return it->second;
}

std::vector<SServerNode> ServerNodes::nodes() const
{
    const shared_lock lock(m_mutex);
    const auto        nodeValues = m_nodes | views::values;
    return {nodeValues.begin(), nodeValues.end()};
}

Strings ServerNodes::getNodeNames() const
{
    const shared_lock lock(m_mutex);
    const auto        nodeValues = m_nodes | views::keys;
    return {nodeValues.begin(), nodeValues.end()};
}

void ServerNodes::logMessage(const LogPriority priority, const std::function<std::string()>& output) const
{
    m_cluster->logMessage(priority, output);
}

void ServerNodes::updateNodeRecord(const SServerNode& node, const bool offline) const
{
    if (!m_storage || !m_storage->isPersistent())
    {
        // Persistence is disabled: there is nowhere to store the node record. Reporting that as
        // an error would turn a chosen configuration into a failure in the log - the first thing
        // seen by everyone who starts the broker without Redis.
        return;
    }

    try
    {
        if (!m_storage->getRedis())
        {
            // Persistence is on, so this one is a real failure: the storage the configuration
            // asks for is not there.
            throw Exception("Redis isn't available");
        }

        const xdoc::Document nodeJson;
        const auto           nodeKey = format("node_{}", node->getName());
        auto&                root = *nodeJson.root();
        const auto           nodeState = offline ? 1 : static_cast<int>(node->getState());

        const auto    keys = m_nodes | views::keys;
        const Strings keysStr(keys.begin(), keys.end());
        const auto    connectedNodes = nodeState == 1 ? "" : keysStr.join(",");

        const auto   settingsNode = nodeJson.root()->findOrCreate("settings");
        const Buffer settingsNodeData(node->getNodeSettings().toString());
        xdoc::Node::importJson(settingsNode, settingsNodeData);

        root.set("updated", DateTime::Now().isoDateTimeString());
        root.set("connected_nodes", connectedNodes);

        Buffer nodeData;
        nodeJson.exportTo(xdoc::DataFormat::JSON, nodeData);
        m_storage->getRedis()->setValue(nodeKey, nodeData);

        // TODO:
        // When a node goes offline, its sessions should be re-assigned to other nodes
        // so they still receive messages while offline.
    }
    catch (const Exception& e)
    {
        logMessage(LogPriority::Error, [error = e.what()]
                   {
                       return format("Can't store node record: {}", error);
                   });
    }
}

void ServerNodes::storeNodeRecord(const SServerNode& node) const
{
    updateNodeRecord(node, false);
}

void ServerNodes::setNodeRecordToOffline(const SServerNode& node) const
{
    updateNodeRecord(node, true);
}
