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

#include "ServerNode.h"
#include "Server.h"
#include "common/mqtt/PublishMessage.h"

using namespace std;
using namespace sptk;

using namespace xmq;
using namespace cluster;

ServerNode::ServerNode(Server* server, CServerNode nodeSettings, const Topics& clusterTopics)
    : m_server(server)
    , m_nodeSettings(std::move(nodeSettings))
    , m_clusterTopics(clusterTopics)
{
    const unique_lock lock(m_mutex);

    stringstream clientId;
    clientId << "node_" << m_server->getNodeName() << "_" << m_nodeSettings.m_node_name.asString();
    m_credentials = make_shared<ConnectCredentials>(clientId.str(), "cluster", m_server->getClusterPassword());
    if (m_nodeSettings.m_xmq_version.isNull())
    {
        m_nodeSettings.m_xmq_version = Server::getVersion();
    }

    if (m_nodeSettings.m_node_state.isNull())
    {
        m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Offline);
    }
}

ReasonCode ServerNode::connect(const bool cluster)
{
    const unique_lock lock(m_mutex);

    m_isClusterNode = cluster;
    m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Offline);

    auto destinationHost = make_unique<Host>(m_nodeSettings.m_host_port);

    if (destinationHost->port() == 0)
    {
        destinationHost = make_unique<Host>(m_nodeSettings.m_host_port, static_cast<uint16_t>(1883));
    }

    const client::ConnectParameters connectParameters;

    const auto messageProperties = make_shared<MessageProperties>();
    messageProperties->setUserProperty("origin-node", m_server->getNodeName());

    shared_ptr<SSLKeys> sslKeys;
    if (m_nodeSettings.m_encrypted.asBool())
    {
        const auto& keysData = m_nodeSettings.m_ssl_keys;

        // Verification follows the configured depth, as it does for bridges and listeners. At
        // zero the link is encrypted and the peer unverified, which between brokers means any
        // machine that can answer on the address is accepted as the node.
        const auto verifyDepth = keysData.m_verify_depth.asInteger();
        const auto verifyMode = verifyDepth ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;

        sslKeys = make_shared<SSLKeys>(
            keysData.m_keyfile.asString().c_str(),
            keysData.m_certfile.asString().c_str(),
            "",
            keysData.m_cafile.asString().c_str(),
            verifyMode,
            verifyDepth);
    }

    const auto rc = m_mqttClient.connect(*destinationHost, *m_credentials, connectParameters, ProtocolVersion::MqttV5, messageProperties, sslKeys);

    if (rc == ReasonCode::Success)
    {
        m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Standalone);
        m_mqttClient.subscribe("$CLUSTER/#");
    }

    return rc;
}

void ServerNode::sendAttachNodeRequest()
{
    m_server->getSettings()->m_cluster.m_redis_uri = m_server->getSettings()->m_persistence.m_redis_uri.asString();
    publish(Command::AttachNodeRequest, m_server->getSettings()->m_cluster);
}

void ServerNode::sendDetachNodeRequest()
{
    publish(Command::DetachNodeRequest, m_server->getSettings()->m_cluster);
}

void ServerNode::disconnect() const
{
    const unique_lock lock(m_mutex);
    m_mqttClient.disconnect();
}

bool ServerNode::isConnected() const
{
    const shared_lock lock(m_mutex);
    return m_mqttClient.isConnected();
}

std::string ServerNode::getName() const
{
    const shared_lock lock(m_mutex);
    return m_nodeSettings.m_node_name.asString().c_str();
}

ServerNodeState ServerNode::getState() const
{
    const shared_lock lock(m_mutex);

    return static_cast<ServerNodeState>(m_nodeSettings.m_node_state.asInteger());
}

void ServerNode::setState(const ServerNodeState state)
{
    const unique_lock lock(m_mutex);
    m_nodeSettings.m_node_state = static_cast<int>(state);
}

bool ServerNode::isClusterNode() const
{
    const shared_lock lock(m_mutex);
    return m_isClusterNode;
}

MessageId ServerNode::publish(const Command command, const WSComplexType& message)
{
    const auto publishMessage = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(command), string_view(message.toString()));
    return m_mqttClient.publish(*publishMessage);
}

void ServerNode::setRecordId(const RecordId recordId)
{
    m_nodeSettings.m_id = recordId;
    m_server->setNodeId(recordId);
}
