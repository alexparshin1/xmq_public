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
#include "server/Settings/ClusterStore.h"
#include "common/ClusterLinks.h"
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

    const auto& linksSetting = m_server->getSettings()->m_cluster.m_links;
    constexpr int defaultLinks = 4;
    const auto    linkCount = linksSetting.isNull() || linksSetting.asInteger() <= 0 ? defaultLinks : linksSetting.asInteger();
    for (int link = 0; link < linkCount; ++link)
    {
        // The first is named as the only one used to be; the others after it.
        stringstream clientId;
        clientId << "node_" << m_server->getNodeName() << "_" << m_nodeSettings.m_node_name.asString();
        if (link > 0)
        {
            clientId << "#" << link;
        }
        m_credentials.push_back(make_shared<ConnectCredentials>(clientId.str(), "cluster", m_server->getClusterPassword()));
        m_links.push_back(make_unique<client::MqttClient>());
        m_links.back()->useBrokerThreads();
    }
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

    const auto sslKeys = clusterLinkKeys();

    // Only the links that are down: one that broke is opened again, the others are left alone.
    for (size_t link = 0; link < m_links.size(); ++link)
    {
        if (m_links[link]->isConnected())
        {
            continue;
        }
        // A link the other end closed is a client that was connected: put down what is left of
        // that connection before opening a new one.
        m_links[link]->disconnect();

        const auto messageProperties = make_shared<MessageProperties>();
        messageProperties->setUserProperty("origin-node", m_server->getNodeName());
        messageProperties->setUserProperty("cluster-link", to_string(link) + "/" + to_string(m_links.size()));

        const auto rc = m_links[link]->connect(*destinationHost, *m_credentials[link], connectParameters, ProtocolVersion::MqttV5, messageProperties, sslKeys);
        if (rc != ReasonCode::Success)
        {
            m_server->logMessage(LogSubject::ClusterConnections, LogPriority::Warning,
                                 format("Link {} of {} to node '{}' at {}: {}", link, m_links.size(),
                                        string(m_nodeSettings.m_node_name.asString().c_str()), string(destinationHost->toString().c_str()), toString(rc)));
            return rc;
        }
        m_server->logMessage(LogSubject::ClusterConnections, LogPriority::Info,
                             format("Link {} of {} to node '{}' at {} opened.", link, m_links.size(),
                                    string(m_nodeSettings.m_node_name.asString().c_str()), string(destinationHost->toString().c_str())));
        if (link == 0)
        {
            m_links[0]->subscribe("$CLUSTER/#");
        }
    }

    m_nodeSettings.m_node_state = static_cast<int>(ServerNodeState::Standalone);
    return ReasonCode::Success;
}

shared_ptr<SSLKeys> ServerNode::clusterLinkKeys() const
{
    // Advertised peer paths refer to another machine. Cluster connections always use this node's
    // certificate and trust store, including connections opened during mesh discovery.
    m_server->getSettings()->refreshPeerCertificates();
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
        String peerName = m_nodeSettings.m_node_name.asString();
        // The first attach uses a temporary host_port name, before the peer introduces
        // itself. Resolve its registered name by the advertised TLS address.
        if (const auto store = m_server->getSettings()->clusterStore(); store && store->isClusterDatabase())
            for (const auto& peer: store->nodes())
                if (peer.m_hostPort == m_nodeSettings.m_host_port.asString())
                {
                    peerName = peer.m_name;
                    break;
                }
        authorityFile = Settings::peerCertificateAuthority(peerName).string();
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
    auto& clusterSettings = m_server->getSettings()->m_cluster;
    clusterSettings.m_redis_uri = m_server->getSettings()->m_persistence.m_redis_uri.asString();
    clusterSettings.m_database_uri = m_server->getSettings()->m_authentication.m_database_uri.asString();

    // A copy, taken under the cluster's lock: the connection handlers rewrite the node list while
    // this thread serialises the settings, and an entry read half-written made the serialiser throw
    // - out of the rejoin thread, std::terminate, a node that is gone. The copies of the settings
    // are the ones the request is built from.
    const auto cluster = m_server->getCluster();
    publish(Command::AttachNodeRequest, cluster ? cluster->getClusterSettings() : clusterSettings);
}

void ServerNode::sendDetachNodeRequest()
{
    publish(Command::DetachNodeRequest, m_server->getSettings()->m_cluster);
}

void ServerNode::disconnect() const
{
    const unique_lock lock(m_mutex);
    for (const auto& link: m_links)
    {
        link->disconnect();
    }
}

bool ServerNode::isConnected() const
{
    // A node is linked when all its links are: one that broke is opened again by connect().
    const shared_lock lock(m_mutex);
    return ranges::all_of(m_links, [](const auto& link) { return link->isConnected(); });
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

vector<size_t> ServerNode::linksFor(const string& topicFilter) const
{
    if (m_links.size() <= 1 || topicFilter.starts_with("$share/"))
    {
        return {0};
    }
    if (topicFilter.find_first_of("+#") == string::npos)
    {
        return {clusterLinkOf(topicFilter, m_links.size())};
    }
    vector<size_t> all(m_links.size());
    ranges::generate(all, [link = size_t(0)]() mutable { return link++; });
    return all;
}

void ServerNode::subscribe(const std::string& topic)
{
    try
    {
        for (const auto link: linksFor(topic))
        {
            m_links[link]->subscribe(Destination(client::MqttClient::getTopic(topic),
                                                 SubscriptionOptions(Qos::Qos1, SubscribeRetainHandling::DoNotRetain, true)));
        }
    }
    catch (const Exception& e)
    {
        m_server->logMessage(LogSubject::ClusterConnections, LogPriority::Warning,
                             format("Can't subscribe to '{}' on node '{}': {}", topic, getName(), e.what()));
    }
}

void ServerNode::unsubscribe(const std::string& topic)
{
    try
    {
        for (const auto link: linksFor(topic))
        {
            m_links[link]->unsubscribe(Destination(client::MqttClient::getTopic(topic)));
        }
    }
    catch (const Exception& e)
    {
        m_server->logMessage(LogSubject::ClusterConnections, LogPriority::Warning,
                             format("Can't unsubscribe from '{}' on node '{}': {}", topic, getName(), e.what()));
    }
}

MessageId ServerNode::publish(const std::shared_ptr<PublishMessage>& message)
{
    // A link that breaks under a write - the other node stopped, or the network failed - is the
    // link's business, and is noticed and reconnected there. It must not reach the caller: the
    // callers are the cluster's own threads (subscription sender, release worker, the link's
    // receiver answering a join), and an exception leaving one of them ends the process. FreeBSD
    // reports a write to a closed TLS connection this way where Linux does not.
    try
    {
        // A node that is cluster-offline forwards nothing: the cluster has stopped expecting it to
        // serve, its links are closed, and a delivery sent on a stale link is one the sender
        // counts on a node that is not there.
        const auto cluster = m_server->getCluster();
        if (cluster && !cluster->isOnline())
        {
            return 0;
        }

        // Cluster control messages: the first link.
        return m_links[0]->publish(*message);
    }
    catch (const Exception& e)
    {
        m_server->logMessage(LogSubject::ClusterConnections, LogPriority::Warning,
                             format("Can't send to node '{}': {}", getName(), e.what()));
    }
    return 0;
}

MessageId ServerNode::publish(const Command command, const WSComplexType& message)
{
    return publish(make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(command), string_view(message.toString())));
}

void ServerNode::setRecordId(const RecordId recordId)
{
    m_nodeSettings.m_id = recordId;
    m_server->setNodeId(recordId);
}
