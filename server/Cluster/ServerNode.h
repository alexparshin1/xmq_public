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

#include "ClusterTopics.h"
#include "ServerNodeState.h"
#include "base/Message.h"
#include "client/MqttClient.h"
#include "service/CServerNode.h"

namespace xmq {

class Server;

namespace cluster {

/**
 * @brief Server node.
 * Used to send cluster-related messages to other nodes in the cluster
 * and send/receive not cluster-related messages in the cluster.
 */
class XMQ_EXPORT ServerNode final
{
public:
    /**
     * @brief Constructor.
     * @param server            XMQ server.
     * @param nodeSettings      This node settings.
     * @param clusterTopics     Cluster topics.
     */
    ServerNode(Server* server, CServerNode nodeSettings, const Topics& clusterTopics);

    /**
     * @brief Connect to the node.
     * Connect to the MQTT node and introduce this node to the cluster (in cluster mode)
     * and optionally subscribe to the node topics.
     * @param cluster True to connect in cluster mode, false otherwise.
     * @return MQTT connection result(reason) code.
     */
    ReasonCode connect(bool cluster = false);

    /**
     * @brief Begin joining the cluster.
     */
    void sendAttachNodeRequest();

    /**
     * @brief Begin leaving the cluster.
     */
    void sendDetachNodeRequest();

    /**
     * @brief Disconnect the node.
     */
    void disconnect() const;

    /**
     * @brief Get the node connection status.
     * @return this node connection status.
     */
    bool isConnected() const;

    /**
     * @brief Get the name of the node.
     * Provides the name of the server node as defined in its configuration.
     * @return The name of the node as a string.
     */
    std::string getName() const;

    /**
     * @brief Get the state of the node.
     * @return The state of the node.
     */
    ServerNodeState getState() const;

    /**
     * @brief Set the state of the node.
     * @param state             The state of the node.
     */
    void setState(ServerNodeState state);

    /**
     * @brief Checks if the node operates in cluster mode.
     * Determines if the current node is configured to function as part of a cluster.
     * @return True if the node is operating in cluster mode. False otherwise.
     */
    bool isClusterNode() const;

    /**
     * @brief Register a callback for incoming messages.
     * Allows setting a function to handle messages received from the node.
     * @param callback The callback function to be executed when a new message is received.
     *                 The function must accept a `const Message&` as its parameter.
     */
    void onMessage(const PublishMessageCallback& callback)
    {
        m_mqttClient.onMessage(callback);
    }

    /**
     * @brief Subscribes to a specific topic on the node.
     * @param topic The topic to subscribe to.
     */
    void subscribe(const std::string& topic)
    {
        m_mqttClient.subscribe(topic);
    }

    /**
     * @brief Publishes a message to the node.
     * @param message The message to be published.
     */
    MessageId publish(const std::shared_ptr<PublishMessage>& message)
    {
        return m_mqttClient.publish(*message);
    }

    /**
     * @brief Publishes a message to the node.
     * @param command Cluster command.
     * @param message The message to be published.
     */
    MessageId publish(Command command, const sptk::WSComplexType& message);

    /**
     * @brief Simple method for waiting until the internal send/receive queue is empty.
     */
    void flush() const
    {
        m_mqttClient.flush();
    }

    CServerNode getNodeSettings() const
    {
        return m_nodeSettings;
    }

    RecordId getRecordId() const
    {
        return m_nodeSettings.m_id.asInteger();
    }

    void setRecordId(RecordId recordId);

private:
    mutable std::shared_mutex           m_mutex;                           ///< Shared mutex.
    client::MqttClient                  m_mqttClient;                      ///< MQTT client.
    Server*                             m_server;                          ///< Owning server instance.
    std::shared_ptr<ConnectCredentials> m_credentials;                     ///< Bridge MQTT client credentials (ID, username, password).
    CServerNode                         m_nodeSettings;                    ///< Bridge configuration.
    sptk::DateTime                      m_created {sptk::DateTime::Now()}; ///< Node creation date.
    bool                                m_isClusterNode {false};           ///< True for cluster nodes.
    const Topics&                       m_clusterTopics;                   ///< Cluster topics.
};

using SServerNode = std::shared_ptr<ServerNode>;

} // namespace cluster
} // namespace xmq
