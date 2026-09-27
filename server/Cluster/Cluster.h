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

#pragma once

#include "../Settings/Settings.h"
#include "../Subscription/Subscription.h"

#include "ClusterTopics.h"
#include "ServerNodes.h"
#include "SubscriptionClient.h"
#include "base/xmq.h"

namespace xmq {
class SubscriptionManager;

class Server;

namespace cluster {
class ServerNode;
/**
 * @brief Server nodes connected to this server.
 */
class XMQ_EXPORT Cluster final
{
public:
    /**
     * @brief Constructor.
     * @param server            XMQ server.
     */
    explicit Cluster(Server* server);

    /**
     * @brief Destructor
     */
    ~Cluster() = default;

    /**
     * @brief Get the cluster time.
     * @return The cluster time.
     */
    sptk::DateTime getClusterTime() const;

    /**
     * @brief Join the cluster using a specified node.
     * @param clusterNodeHost One of the cluster nodes.
     */
    /**
     * @brief Attach this node to the cluster another node belongs to.
     * @param clusterNodeHost   Address of a node already in the cluster.
     * @param encrypted         True when that address is served over TLS, in which case this
     *                          node's own keys and trust settings are used for the link.
     */
    void joinCluster(const sptk::Host& clusterNodeHost, bool encrypted = false);

    /**
     * @brief Initiates the connection of this server to the cluster.
     *
     * This method performs the handshake process required to connect this server
     * to the cluster. It sets the current server mode to "Connecting", establishes
     * MQTT-level connections to all cluster nodes, and notifies all nodes about
     * the attachment of this server.
     */
    static void connectToCluster();

    /**
     * @brief Disconnects this server from the connected cluster.
     *
     * This method notifies all nodes within the cluster about the detachment
     * of this server by sending a DetachNode command. It is used to cleanly
     * deregister the server from the cluster and to update the state of all
     * nodes accordingly.
     */
    void detachCluster();

    /**
     * @brief Log a message
     * @param logPriority Log priority
     * @param output Calback returning log message
     */
    void logMessage(sptk::LogPriority logPriority, const sptk::Logger::OutputString& output) const;

    /**
     * @brief Count nodes matching the node name filter.
     * @param nodeNameFilter Regular expression to match node names, or empty string to match all nodes.
     * @return Number of matched nodes.
     */
    size_t   getConnectedNodeCount(const std::string& nodeNameFilter = {}) const;
    /// Whether any node is connected; takes no lock.
    bool hasNodes() const noexcept
    {
        return m_connectedNodes.anyNodes();
    }
    size_t   getClusterNodeCount(const std::string& nodeNameFilter = {}) const;
    CCluster getClusterSettings() const;

    void connectClusterNode(std::string_view nodeName);
    void disconnectClusterNode(std::string_view nodeName);

    /**
     * @brief Get the host of this cluster node.
     * @return Get the host of this cluster node.
     */
    sptk::Host getNodeHost() const;

    /**
     * @brief Get the manager of cluster-specific topics.
     * @return The manager of the cluster-specific topic.
     */
    const Topics& getClusterTopics() const
    {
        return m_clusterTopics;
    }

    /**
     * @brief Send a cluster command to each connected cluster node.
     * @param clusterCommand    Cluster command.
     * @param content           Optional message content.
     */
    void notifyAllNodes(Command clusterCommand, const std::string& content = {}) const;

    /**
     * @brief Accepts and processes an incoming cluster message.
     *
     * @param publishMessage    The incoming message to be processed.
     * @param sender            The sender of the message.
     */
    void acceptClusterMessage(const SPublishMessage& publishMessage, const std::string& sender);

    [[nodiscard]] sptk::Strings getClusterNodesNames() const;
    [[nodiscard]] Server*       getServer() const;

private:
    mutable std::shared_mutex            m_mutex;                  ///< Mutex that protects access to internal data
    Server*                              m_server;                 ///< Server.
    std::shared_ptr<Settings>            m_settings;               ///< Server settings.
    std::shared_ptr<SubscriptionManager> m_subscriptionManager;    ///< Subscription manager.
    std::shared_ptr<sptk::LogEngine>     m_logEngine;              ///< Log engine.
    SServerNode                          m_thisNode;               ///< This node settings.
    ServerNodes                          m_connectedNodes;         ///< Connected nodes
    Topics                               m_clusterTopics;          ///< Cluster topics.
    SSubscriptionClient                  m_subscriptionClient;     ///< Cluster subscription client.
    sptk::Flag                           m_attachResponseReceived; ///< Flag: Attach request response received.

    /**
     * @brief Connect to all nodes on the MQTT level
     */
    void mqttConnectAllNodes();

    SNode connectNode(const CServerNode& nodeSettings);
    SNode findClusterNode(const std::string& nodeName) const;
    void  updateClusterInfo() const;
    void  registerConnectedNode(const SNode& node);
    void  deregisterConnectedNode(const SNode& node);
    void  disconnectNode(const SNode& node);

    static void subscribeToConnectedNode(const SNode& node, const sptk::Strings& topics);

    /**
     * @brief Process the received join request message.
     * @param message The join request message.
     */
    void onAttachNodeRequest(const SPublishMessage& message);

    /**
     * @brief Process the received join response message.
     * @param message The join response message.
     */
    void onAttachNodeResponse(const SPublishMessage& message);

    /**
     * @brief Process the received detach request message.
     * @param message The detach request message.
     */
    void onDetachNodeRequest(const SPublishMessage& message);

    /**
     * @brief Process the received detach response message.
     * @param message The detach request message.
     */
    void onDetachNodeResponse(const SPublishMessage& message);

    /**
     * @brief Request to disconnect the client from the broker.
     * @param message           Contains the client name.
     * @param sender            Message sender.
     */
    void onDisconnectClientRequest(const SPublishMessage& message, const std::string& sender) const;

    /**
     * @brief Accept the incoming cluster message.
     * @param message           Cluster message.
     * @param sender            Sender of the message.
     */
    void   processClusterMessage(const SPublishMessage& message, const std::string& sender);
    size_t getNodeCountUnlocked(const ServerNodes& nodes, const std::string& nodeNameFilter) const;

    template<typename T>
    T deserializeMessage(const SPublishMessage& message)
    {
        sptk::xdoc::Document document;
        document.load(message->payload());
        T result;
        result.load(document.root());
        return result;
    }
};


using SCluster = std::shared_ptr<Cluster>;

} // namespace cluster
} // namespace xmq
