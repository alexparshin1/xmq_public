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

#include "Server.h"
#include "common/mqtt/PublishMessage.h"
#include "service/CServerNode.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace cluster;

Cluster::Cluster(Server* server)
    : m_server(server)
    , m_settings(server->getSettings())
    , m_subscriptionManager(server->getSubscriptionManager())
    , m_logEngine(server->getLogEngine())
    , m_connectedNodes(this, server->getStorage())
    , m_clusterTopics(server->getTopicManager())
    , m_subscriptionClient(make_shared<SubscriptionClient>(server))
{
    auto& thisNodeSettings = m_settings->m_cluster.m_this_node;
    thisNodeSettings.m_xmq_version = Server::getVersion();
    thisNodeSettings.m_encrypted = true;
    thisNodeSettings.m_node_state = static_cast<int>(ServerNodeState::Standalone);
    m_thisNode = make_shared<ServerNode>(m_server, m_settings->m_cluster.m_this_node, m_clusterTopics);
    m_connectedNodes.storeNodeRecord(m_thisNode);
}

DateTime Cluster::getClusterTime() const
{
    // Delegated rather than cached here: the storage clock is what every node in the cluster
    // compares against, and Storage re-samples the offset as it ages. A copy taken once when the
    // cluster was constructed would drift for as long as the process ran.
    if (const auto storage = m_server->getStorage())
    {
        return storage->getCurrentTime();
    }

    // No persistence configured, so there is no shared clock to anchor to and nothing that needs
    // one: without storage there is no cross-node state to time out.
    return DateTime::Now();
}

void Cluster::joinCluster(const Host& clusterNodeHost, const bool encrypted)
{
    if (!encrypted)
    {
        throw Exception("Cluster connections require MQTT+SSL.");
    }

    // The address is the peer's TLS listener. Keys and trust settings belong to the local node.
    CServerNode nodeSettings;
    nodeSettings.m_node_name = format("{}_{}", clusterNodeHost.hostname(), clusterNodeHost.port());
    nodeSettings.m_host_port = clusterNodeHost.toString();
    nodeSettings.m_encrypted = true;
    nodeSettings.m_ssl_keys = m_settings->m_connections.m_ssl_keys;
    const auto serverNode = make_shared<ServerNode>(m_server, nodeSettings, m_clusterTopics);

    m_attachResponseReceived = false;
    if (serverNode->connect() != ReasonCode::Success)
    {
        throw Exception("Cannot connect to the cluster TLS listener.");
    }
    serverNode->sendAttachNodeRequest();
    if (!m_attachResponseReceived.wait_for(true, 1000ms))
    {
        logMessage(LogPriority::Error, [this, clusterNodeHost]
                   {
                       stringstream str;
                       str << "Node " << m_server->getNodeName() << " couldn't connect to node " << clusterNodeHost.hostname() << ":" << clusterNodeHost.port();
                       return str.str();
                   });
    }
}

void Cluster::connectToCluster()
{
    /*
    logMessage(LogPriority::Info, []
               {
                   return "─────────────[Begin cluster handshake]────────────────────";
               });

    m_server->setMode(NodeStatus::Connecting);

    mqttConnectAllNodes();
    notifyAllNodes(Command::AttachNode);

    logMessage(LogPriority::Info, []
               {
                   return "─────────────[Finish cluster handshake]────────────────────";
               });
    */
}

void Cluster::detachCluster()
{
    notifyAllNodes(Command::DetachNodeRequest);
}

SNode Cluster::findClusterNode(const std::string& nodeName) const
{
    return m_connectedNodes.findNode(nodeName);
}

void Cluster::updateClusterInfo() const
{
    m_settings->m_cluster.m_nodes.clear();
    m_settings->m_cluster.m_this_node.m_node_name = m_server->getNodeName();
    for (const auto& node: m_connectedNodes.nodes())
    {
        m_settings->m_cluster.m_nodes.push_back(node->getNodeSettings());
    }
}

void Cluster::registerConnectedNode(const SNode& node)
{
    if (!node->isConnected())
    {
        return;
    }

    try
    {
        unique_lock lock(m_mutex);

        m_connectedNodes.addNode(node, StoreNodeMode::InsertOnly);

        updateClusterInfo();

        logMessage(LogPriority::Debug,
                   [&node]
                   {
                       return "Connected node '" + node->getName() + "'.";
                   });
    }
    catch (const Exception& e)
    {
        logMessage(LogPriority::Error,
                   [&node, e]
                   {
                       return format("Can't register connected node '{}': {}", node->getName(), e.what());
                   });
    }
}

void Cluster::deregisterConnectedNode(const SNode& node)
{
    unique_lock lock(m_mutex);

    m_connectedNodes.setNodeToOffline(node);

    updateClusterInfo();
}

void Cluster::subscribeToConnectedNode(const SNode& node, const Strings& topics)
{
    for (const auto& topic: topics)
    {
        node->subscribe(topic);
    }
}

SNode Cluster::connectNode(const CServerNode& nodeSettings)
{
    const string nodeName = nodeSettings.m_node_name.asString().c_str();
    if (nodeName == m_server->getNodeName())
    {
        return nullptr;
    }

    auto node = findClusterNode(nodeName);
    if (!node)
    {
        node = make_shared<ServerNode>(m_server, nodeSettings, m_clusterTopics);
    }

    if (node->isConnected())
    {
        return node;
    }

    ReasonCode rc;
    try
    {
        rc = node->connect();
    }
    catch (const Exception& e)
    {
        // A record that is not encrypted, or a node there is nothing to verify against. Neither
        // is cured by retrying, and neither should take the discovery of the other nodes with it.
        logMessage(LogPriority::Error, [&node, &e]
                   {
                       return "Couldn't connect to node '" + node->getName() + "': " + e.what();
                   });
        return nullptr;
    }

    if (rc == ReasonCode::Success)
    {
        registerConnectedNode(node);
        node->onMessage([this, node](const SPublishMessage& message)
                        {
                            acceptClusterMessage(message, node->getName());
                        });
        publishSubscriptionSnapshot(node);

        return node;
    }

    logMessage(LogPriority::Warning, [&node, rc]
               {
                   return "Couldn't connect to node '" + node->getName() + "': " + toString(rc) + ".";
               });

    return nullptr;
}

void Cluster::disconnectNode(const SNode& node)
{
    node->disconnect();
    deregisterConnectedNode(node);
    {
        const scoped_lock lock(m_subscriptionMutex);
        m_nodeSubscriptions.erase(node->getName());
    }
    logMessage(LogPriority::Debug, [&node]
               {
                   return "Disconnected node '" + node->getName() + "'.";
               });
}

void Cluster::logMessage(const LogPriority logPriority, const Logger::OutputString& output) const
{
    m_server->logMessage(LogSubject::ClusterEvents, logPriority, output);
}

void Cluster::mqttConnectAllNodes()
{
    for (const auto& nodeSettings: m_settings->m_cluster.m_nodes)
    {
        try
        {
            auto node = connectNode(nodeSettings);
            subscribeToConnectedNode(node, {"#"});
        }
        catch (const Exception& e)
        {
            logMessage(LogPriority::Error,
                       [&nodeSettings, &e]
                       {
                           stringstream str;
                           str << "Couldn't connect to node '" << nodeSettings.m_node_name.asString()
                               << "' at " << nodeSettings.m_host_port.asString() << ": " << e.what();
                           return str.str();
                       });
        }
    }
}

void Cluster::notifyAllNodes(const Command clusterCommand, const string& content) const
{
    scoped_lock lock(m_mutex);
    for (const auto& node: m_connectedNodes.nodes())
    {
        if (!node->isConnected())
        {
            continue;
        }
        if (node->getName() == m_server->getNodeName())
        {
            continue;
        }

        using enum Command;
        switch (clusterCommand)
        {
            case DetachNodeRequest:
                node->publish(make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(DetachNodeRequest), string_view(m_server->getNodeName())));
                break;
            case DisconnectClient:
                node->publish(make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(DisconnectClient), string_view(content)));
                break;
            default:
                break;
        }
    }
}

void Cluster::onAttachNodeRequest(const SPublishMessage& message)
{
    auto attachNodeRequest = deserializeMessage<CCluster>(message);

    if (attachNodeRequest.m_redis_uri.asString() != m_settings->m_persistence.m_redis_uri.asString())
    {
        throw Exception("Cluster nodes must use the same database.");
    }

    CCluster existingCluster;
    existingCluster.m_this_node.m_node_name = m_server->getNodeName();

    // Add all connected cluster nodes to the response object:
    for (const auto& node: m_connectedNodes.nodes())
    {
        existingCluster.m_nodes.push_back(node->getNodeSettings());
    }

    // Add the current node to the response object:
    existingCluster.m_nodes.push_back(m_thisNode->getNodeSettings());

    // Connect the joining node to this node cluster:
    if (const auto connectedNode = connectNode(attachNodeRequest.m_this_node))
    {
        connectedNode->publish(Command::AttachNodeResponse, existingCluster);
        connectedNode->setState(ServerNodeState::ConnectedToCluster);
        m_connectedNodes.addNode(connectedNode, StoreNodeMode::InsertOnly);
        subscribeToConnectedNode(connectedNode, {"#"});
    }

    logMessage(LogPriority::Info, [&attachNodeRequest]
               {
                   return format("Attached to cluster node [{}]", attachNodeRequest.m_this_node.m_node_name.asString().c_str());
               });

    m_thisNode->setState(ServerNodeState::ConnectedToCluster);
    m_connectedNodes.storeNodeRecord(m_thisNode);
}

void Cluster::onAttachNodeResponse(const SPublishMessage& message)
{
    auto clusterSettings = deserializeMessage<CCluster>(message);

    CCluster attachNodeRequest;
    attachNodeRequest.m_this_node = m_settings->m_cluster.m_this_node;
    attachNodeRequest.m_nodes.push_back(attachNodeRequest.m_this_node);

    for (const auto& nodeSettings: clusterSettings.m_nodes)
    {
        const auto connectedNode = connectNode(nodeSettings);

        // Don't send the join request to the server that sent the join response:
        if (clusterSettings.m_this_node.m_node_name.asString() != nodeSettings.m_node_name.asString())
        {
            connectedNode->publish(Command::AttachNodeResponse, attachNodeRequest);
        }

        m_connectedNodes.addNode(connectedNode, StoreNodeMode::InsertOnly);
        subscribeToConnectedNode(connectedNode, {"#"});
    }

    m_thisNode->setState(ServerNodeState::ConnectedToCluster);
    m_connectedNodes.storeNodeRecord(m_thisNode);

    logMessage(LogPriority::Info, [&clusterSettings]
               {
                   return format("Attached to cluster node [{}]", clusterSettings.m_this_node.m_node_name.asString().c_str());
               });
}

void Cluster::onDetachNodeRequest(const SPublishMessage& message)
{
    const string nodeName(message->payload());
    const auto   node = findClusterNode(nodeName);
    if (node == nullptr)
    {
        return;
    }
    const auto detachMessage = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::DetachNodeResponse), string_view(m_server->getNodeName()));
    node->publish(detachMessage);
    node->flush();
    disconnectNode(node);
}

void Cluster::onDetachNodeResponse(const SPublishMessage& message)
{
    const string nodeName(message->payload());
    const auto   node = findClusterNode(nodeName);
    if (node == nullptr)
    {
        return;
    }
    disconnectNode(node);
}

void Cluster::onDisconnectClientRequest(const SPublishMessage& message, const string& sender) const
{
    if (sender != m_server->getNodeName() && sender != "internal")
    {
        logMessage(LogPriority::Debug, [&message]
                   {
                       return format("Process disconnect {}", message->toString());
                   });

        m_server->closeSession(string(message->payload()));
    }
}

void Cluster::acceptClusterMessage(const SPublishMessage& publishMessage, const string& sender)
{
    if (publishMessage->destination()->isCluster())
    {
        // Cluster message
        processClusterMessage(publishMessage, sender);
    }
    else
    {
        // Bridged message
        publishMessage->setSender(sender);
        publishMessage->setSourceNode(sender);
        m_server->publishMessage(publishMessage);
    }
}

void Cluster::updateLocalSubscription(const string_view topicFilter, const bool subscribed)
{
    const scoped_lock subscriptionLock(m_subscriptionMutex);
    if (subscribed)
    {
        m_localSubscriptions.emplace(topicFilter);
    }
    else
    {
        m_localSubscriptions.erase(string(topicFilter));
    }

    // The saved set is sent in full when a peer joins. With no registered peers, avoid rebuilding
    // this growing snapshot once for every new local filter; doing so makes topic-heavy CONNECT
    // bursts quadratic even when clustering is disabled.
    if (!m_connectedNodes.anyNodes())
    {
        return;
    }

    string payload;
    for (const auto& filter: m_localSubscriptions)
    {
        payload.append(filter);
        payload.push_back('\n');
    }

    const scoped_lock nodesLock(m_mutex);
    for (const auto& node: m_connectedNodes.nodes())
    {
        if (!node->isConnected() || node->getName() == m_server->getNodeName())
        {
            continue;
        }
        if (subscribed)
        {
            node->subscribe(string(topicFilter));
        }
        else
        {
            node->unsubscribe(string(topicFilter));
        }
        auto message = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::SubscriptionSnapshot), payload);
        message->setQos(Qos::Qos1);
        node->publish(message);
    }
}

void Cluster::publishSubscriptionSnapshot(const SNode& node) const
{
    const scoped_lock lock(m_subscriptionMutex);
    string payload;
    for (const auto& filter: m_localSubscriptions)
    {
        node->subscribe(filter);
        payload.append(filter);
        payload.push_back('\n');
    }
    auto message = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::SubscriptionSnapshot), payload);
    message->setQos(Qos::Qos1);
    node->publish(message);
}

set<string> Cluster::getNodeSubscriptions(const string_view nodeName) const
{
    const scoped_lock lock(m_subscriptionMutex);
    if (nodeName == m_server->getNodeName())
    {
        return m_localSubscriptions;
    }
    if (const auto it = m_nodeSubscriptions.find(string(nodeName));
        it != m_nodeSubscriptions.end())
    {
        return it->second;
    }
    return {};
}

Strings Cluster::getClusterNodesNames() const
{
    const scoped_lock lock(m_mutex);
    Strings           nodeNames;
    for (const auto& node: m_connectedNodes.nodes())
    {
        nodeNames.push_back(node->getName());
    }
    return nodeNames;
}

Server* Cluster::getServer() const
{
    return m_server;
}

void Cluster::processClusterMessage(const SPublishMessage& message, const string& sender)
{
    using enum Command;
    switch (m_clusterTopics.getCommand(message->destination()))
    {
        case AttachNodeRequest:
        case AttachNode:
            onAttachNodeRequest(message);
            break;
        case AttachNodeResponse:
            m_attachResponseReceived = true;
            onAttachNodeResponse(message);
            break;
        case DetachNodeRequest:
            onDetachNodeRequest(message);
            break;
        case DetachNodeResponse:
            onDetachNodeResponse(message);
            break;
        case DisconnectClient:
            onDisconnectClientRequest(message, sender);
            break;
        case SubscriptionSnapshot:
        {
            set<string> filters;
            string_view payload = message->payload();
            while (!payload.empty())
            {
                const auto separator = payload.find('\n');
                const auto filter = payload.substr(0, separator);
                if (!filter.empty())
                {
                    filters.emplace(filter);
                }
                if (separator == string_view::npos)
                {
                    break;
                }
                payload.remove_prefix(separator + 1);
            }
            const scoped_lock lock(m_subscriptionMutex);
            m_nodeSubscriptions[sender] = std::move(filters);
            break;
        }
        case Unknown:
            logMessage(LogPriority::Error, [&message]
                       {
                           return format("Unknown cluster command: {}", message->toString());
                       });
            break;
        case NodeDetached:
            logMessage(LogPriority::Debug, [&message]
                       {
                           return format("Node detached: {}", message->toString());
                       });
            break;
    }
}

size_t Cluster::getNodeCountUnlocked(const ServerNodes& nodes, const string& nodeNameFilter) const
{
    if (nodeNameFilter.empty())
    {
        return m_connectedNodes.size();
    }

    const RegularExpression matchNodeNames(nodeNameFilter);

    size_t count = 0;

    for (const auto& node: nodes.nodes())
    {
        if (matchNodeNames.matches(node->getName()))
        {
            ++count;
        }
    }

    return count;
}

CCluster Cluster::getClusterSettings() const
{
    scoped_lock lock(m_mutex);
    return m_settings->m_cluster;
}

size_t Cluster::getConnectedNodeCount(const string& nodeNameFilter) const
{
    scoped_lock lock(m_mutex);
    return getNodeCountUnlocked(m_connectedNodes, nodeNameFilter);
}

size_t Cluster::getClusterNodeCount(const std::string& nodeNameFilter) const
{
    scoped_lock lock(m_mutex);
    return getNodeCountUnlocked(m_connectedNodes, nodeNameFilter);
}

void Cluster::connectClusterNode(string_view)
{
    // Not implemented yet
}

void Cluster::disconnectClusterNode(string_view)
{
    // Not implemented yet
}

Host Cluster::getNodeHost() const
{
    return Host {m_settings->m_cluster.m_this_node.m_host_port.asString()};
}
