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
#include "server/Settings/Settings.h"
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

ReasonCode ServerNode::connect()
{
    const unique_lock lock(m_mutex);

    if (!m_nodeSettings.m_encrypted.asBool())
    {
        throw Exception("Cluster connections require MQTT+SSL.");
    }

    m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Offline);

    auto destinationHost = make_unique<Host>(m_nodeSettings.m_host_port);

    if (destinationHost->port() == 0)
    {
        destinationHost = make_unique<Host>(m_nodeSettings.m_host_port, static_cast<uint16_t>(8883));
    }

    const client::ConnectParameters connectParameters;

    const auto messageProperties = make_shared<MessageProperties>();
    messageProperties->setUserProperty("origin-node", m_server->getNodeName());

    const auto sslKeys = clusterLinkKeys();

    const auto rc = m_mqttClient.connect(*destinationHost, *m_credentials, connectParameters, ProtocolVersion::MqttV5, messageProperties, sslKeys);

    if (rc == ReasonCode::Success)
    {
        m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Standalone);
        m_mqttClient.subscribe("$CLUSTER/#");
    }

    return rc;
}

shared_ptr<SSLKeys> ServerNode::clusterLinkKeys() const
{
    // Advertised peer paths refer to another machine. Cluster connections always use this node's
    // certificate and trust store, including connections opened during mesh discovery.
    const auto& keysData = m_server->getSettings()->m_connections.m_ssl_keys;

    const auto [nodeCertificate, nodeKey] = Settings::nodeKeyFiles();
    const auto certificateFile = keysData.m_certfile.asString().empty()
                                     ? nodeCertificate.string()
                                     : keysData.m_certfile.asString();
    const auto privateKeyFile = keysData.m_keyfile.asString().empty()
                                    ? nodeKey.string()
                                    : keysData.m_keyfile.asString();

    // Between brokers there is usually no certificate authority: each node holds the others'
    // certificates in the peers directory, and a self-signed certificate vouches for itself.
    auto authorityFile = keysData.m_cafile.asString();
    if (authorityFile.empty())
    {
        authorityFile = Settings::buildPeerCertificateBundle().string();
    }

    // Unlike a bridge, a cluster link is never left unverified. It carries the cluster password
    // and the storage address, and whatever answers in a node's place is handed both.
    if (authorityFile.empty())
    {
        throw Exception(format("Cannot verify node '{}': no trusted certificates. Add its "
                               "certificate to {}.",
                               m_nodeSettings.m_node_name.asString().c_str(),
                               Settings::peerCertificatesDirectory().string()));
    }

    const auto verifyDepth = max<int64_t>(keysData.m_verify_depth.asInteger(), 1);

    return make_shared<SSLKeys>(privateKeyFile.c_str(), certificateFile.c_str(), "",
                                authorityFile.c_str(), SSL_VERIFY_PEER, static_cast<int>(verifyDepth));
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
