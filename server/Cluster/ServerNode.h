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
     * @brief Connect to the node over MQTT+SSL and subscribe to its cluster topics.
     *
     * The node must present a certificate this node trusts: one in the certificates' peers
     * directory, or one issued by the configured certificate authority.
     *
     * @return MQTT connection result(reason) code.
     * @throws sptk::Exception  When the node record is not encrypted, or nothing is trusted to
     *                          verify the node against.
     */
    ReasonCode connect();

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
        m_mqttClient.subscribe(Destination(client::MqttClient::getTopic(topic),
                                           SubscriptionOptions(Qos::Qos1, SubscribeRetainHandling::RetainAlways, true)));
    }

    /**
     * @brief Unsubscribes from a specific topic on the node.
     * @param topic The topic to unsubscribe from.
     */
    void unsubscribe(const std::string& topic)
    {
        m_mqttClient.unsubscribe(Destination(client::MqttClient::getTopic(topic)));
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
    /**
     * @brief The keys a cluster link is opened with.
     *
     * This node's own certificate, and the certificates it trusts: the configured certificate
     * authority, or the peers directory. Verification is not optional on a cluster link.
     *
     * @return the keys.
     * @throws sptk::Exception  When nothing is trusted to verify a node against.
     */
    std::shared_ptr<sptk::SSLKeys> clusterLinkKeys() const;

    mutable std::shared_mutex           m_mutex;                           ///< Shared mutex.
    client::MqttClient                  m_mqttClient;                      ///< MQTT client.
    Server*                             m_server;                          ///< Owning server instance.
    std::shared_ptr<ConnectCredentials> m_credentials;                     ///< Bridge MQTT client credentials (ID, username, password).
    CServerNode                         m_nodeSettings;                    ///< Bridge configuration.
    sptk::DateTime                      m_created {sptk::DateTime::Now()}; ///< Node creation date.
    const Topics&                       m_clusterTopics;                   ///< Cluster topics.
};

using SServerNode = std::shared_ptr<ServerNode>;

} // namespace cluster
} // namespace xmq
