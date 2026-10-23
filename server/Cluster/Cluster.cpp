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
#include "NodeIdentity.h"
#include "storage/RedisConnection.h"
#include "server/Settings/ClusterStore.h"
#include "server/OSService.h"
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
    m_invitations.setSettings(m_settings);
    auto& thisNodeSettings = m_settings->m_cluster.m_this_node;
    thisNodeSettings.m_xmq_version = Server::getVersion();
    thisNodeSettings.m_encrypted = true;
    thisNodeSettings.m_node_state = static_cast<int>(ServerNodeState::Standalone);
    m_thisNode = make_shared<ServerNode>(m_server, m_settings->m_cluster.m_this_node, m_clusterTopics);

    // Kept beside the configuration. Without one there is nowhere to keep it, and the node is a new
    // one every start - which only a test that never restarts a node does.
    if (const auto configuration = m_settings->configurationPath(); !configuration.empty())
    {
        m_nodeId = NodeIdentity::load(filesystem::path(configuration).replace_filename(NodeIdentity::FileName));
    }
    else
    {
        m_nodeId = NodeIdentity::generate();
    }
    m_connectedNodes.storeNodeRecord(m_thisNode);
    m_subscriptionSender = thread([this] { sendSubscriptions(); });
    m_releaseWorker = thread([this] { releaseSessions(); });
}

Cluster::~Cluster()
{
    stopSubscriptionSender();
    stopReleaseWorker();
}

void Cluster::stopReleaseWorker()
{
    {
        const scoped_lock lock(m_releaseMutex);
        m_stopping = true;
    }
    m_releaseAdded.notify_all();
    if (m_releaseWorker.joinable() && m_releaseWorker.get_id() != this_thread::get_id())
    {
        m_releaseWorker.join();
    }
}

void Cluster::releaseSessions()
{
    unique_lock lock(m_releaseMutex);
    while (!m_stopping)
    {
        m_releaseAdded.wait(lock, [this] { return m_stopping || !m_releaseRequests.empty() || !m_sessionTasks.empty(); });
        while (!m_sessionTasks.empty() && !m_stopping)
        {
            const auto task = std::move(m_sessionTasks.front());
            m_sessionTasks.pop_front();
            lock.unlock();
            try
            {
                task();
            }
            catch (const Exception& e)
            {
                logMessage(LogPriority::Error, [&e] { return format("Cluster session task failed: {}", e.what()); });
            }
            lock.lock();
        }
        while (!m_releaseRequests.empty() && !m_stopping)
        {
            const auto [clientId, nodeId] = m_releaseRequests.front();
            m_releaseRequests.pop_front();
            lock.unlock();
            try
            {
                logMessage(LogPriority::Debug, [&clientId] { return format("Letting session {} go.", clientId); });
                // What matches it from now on goes to the node it moves to, named, until that node
                // has subscribed for it - recorded before it is let go, so that nothing falls between.
                if (auto* coordinator = m_coordinatorView.load())
                {
                    if (const auto session = m_server->getClientSession(clientId))
                    {
                        MovedSession moved {coordinator->nodeName(nodeId), {}, chrono::steady_clock::now()};
                        for (const auto& filter: views::keys(session->getSubscribedTo()))
                        {
                            if (!filter.starts_with("$share/"))
                            {
                                moved.m_filters.emplace_back(filter);
                            }
                        }
                        if (!moved.m_nodeName.empty() && !moved.m_filters.empty())
                        {
                            const scoped_lock movedLock(m_movedMutex);
                            m_movedSessions[clientId] = std::move(moved);
                        }
                    }
                }
                if (!m_server->releaseSession(clientId))
                {
                    logMessage(LogPriority::Warning, [&clientId]
                               {
                                   return format("Session {}: Redis did not confirm its writes before the handover.", clientId);
                               });
                }
                if (auto* coordinator = m_coordinatorView.load())
                {
                    coordinator->handOverSession(clientId, nodeId);
                }
            }
            catch (const Exception& e)
            {
                logMessage(LogPriority::Error, [&clientId, &e] { return format("Can't hand session {} over: {}", clientId, e.what()); });
            }
            lock.lock();
        }
    }
}

void Cluster::queueSessionTask(function<void()> task)
{
    {
        const scoped_lock lock(m_releaseMutex);
        m_sessionTasks.push_back(std::move(task));
    }
    m_releaseAdded.notify_one();
}

void Cluster::adoptSessions(const vector<string>& clientIds)
{
    auto* coordinator = m_coordinatorView.load();
    if (coordinator == nullptr)
    {
        return;
    }
    size_t adopted = 0;
    for (const auto& clientId: clientIds)
    {
        // A client back already, on this node or another, holds its session: it is not an orphan.
        if (m_server->getClientSession(clientId) || !coordinator->claimOrphanSession(clientId))
        {
            continue;
        }
        m_server->adoptSession(clientId);
        ++adopted;
    }
    logMessage(LogPriority::Info, [adopted, &clientIds]
               {
                   return format("Adopted {} of {} session(s) of nodes that are gone.", adopted, clientIds.size());
               });
}

void Cluster::dropSessionsOwnedElsewhere()
{
    auto* coordinator = m_coordinatorView.load();
    if (coordinator == nullptr)
    {
        return;
    }
    vector<string> held;
    m_server->getClientSessionManager()->forEach([&held](const SClientSession& session)
                                                 {
                                                     if (!session->isClusterSession() && !session->isCleanSession())
                                                     {
                                                         held.emplace_back(session->getClientId());
                                                     }
                                                 });
    size_t dropped = 0;
    for (const auto& clientId: held)
    {
        if (const auto owner = coordinator->sessionOwner(clientId); !owner.empty() && owner != m_nodeId)
        {
            (void) m_server->releaseSession(clientId);
            ++dropped;
        }
    }
    if (dropped > 0)
    {
        logMessage(LogPriority::Info, [dropped] { return format("Let go of {} session(s) other nodes took over while this one was offline.", dropped); });
    }
}

bool Cluster::claimSession(const string& clientId)
{
    auto* coordinator = m_coordinatorView.load();
    if (coordinator == nullptr || !coordinator->isParticipating())
    {
        return true;
    }

    // Long enough for a node to write out a session with a full queue; short enough to answer the
    // client while it still waits for its CONNACK.
    constexpr auto claimTimeout = chrono::seconds(3);
    constexpr auto askAgainAfter = chrono::milliseconds(500);
    const auto     giveUpAt = chrono::steady_clock::now() + claimTimeout;
    auto           askedAt = chrono::steady_clock::time_point {};
    try
    {
        for (;;)
        {
            const auto holder = coordinator->claimSession(clientId);
            if (holder.empty())
            {
                logMessage(LogPriority::Debug, [&clientId] { return format("Session {} is this node's.", clientId); });
                return true;
            }
            const auto now = chrono::steady_clock::now();
            if (now > giveUpAt)
            {
                break;
            }
            if (now - askedAt >= askAgainAfter)
            {
                askedAt = now;
                if (const auto node = findClusterNode(coordinator->nodeName(holder)); node && node->isConnected())
                {
                    node->publish(make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::ReleaseSession),
                                                                     string_view(clientId + "\n" + m_nodeId)));
                }
            }
            this_thread::sleep_for(10ms);
        }
    }
    catch (const Exception& e)
    {
        logMessage(LogPriority::Error, [&clientId, &e] { return format("Can't claim session {}: {}", clientId, e.what()); });
        return false;
    }

    logMessage(LogPriority::Warning, [&clientId]
               {
                   return format("Session {} is held by another node, which did not let it go in time.", clientId);
               });
    return false;
}

void Cluster::stopSubscriptionSender()
{
    {
        const scoped_lock lock(m_subscriptionMutex);
        m_stopSubscriptionSender = true;
    }
    m_subscriptionWorkAdded.notify_all();
    if (m_subscriptionSender.joinable() && m_subscriptionSender.get_id() != this_thread::get_id())
    {
        m_subscriptionSender.join();
    }
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
    if (!m_settings->m_cluster.m_enabled.isNull() && !m_settings->m_cluster.m_enabled.asBool())
    {
        throw Exception("Accept an invitation before joining this node to the cluster.");
    }
    if (!encrypted)
    {
        throw Exception("Cluster connections require MQTT+SSL.");
    }

    // Admitted first, by itself, through the shared storage: a cluster that is full, or has a node
    // of this name, refuses it before any link is opened. The node it asks then finds it a member.
    // Not when there is no cluster yet: the node asked forms it, as its first member, and this one
    // follows when it answers (onAttachNodeResponse) - admitted first, it would be the senior.
    if (clusterExists())
    {
        startCoordinator();
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
    if (auto* coordinator = m_coordinatorView.load())
    {
        coordinator->leave();
    }
}

void Cluster::startup()
{
    const auto storage = m_server->getStorage();
    const auto enabled = !m_settings->m_cluster.m_enabled.isNull() && m_settings->m_cluster.m_enabled.asBool();
    if (!storage || !storage->isPersistent() || !storage->getRedis())
    {
        if (enabled) throw Exception("Cluster mode requires its shared Redis storage to be available.");
        return;
    }

    RedisConnect redis;
    redis.connect(storage->getRedis()->getRedisUrl());
    const auto membership = Coordinator::membership(redis, m_nodeId);

    if (enabled || (membership == 1 && m_settings->m_cluster.m_enabled.isNull()))
    {
        startCoordinator();
        return;
    }

    if (membership >= 0)
    {
        // The storage is shared with a cluster, and this node is not a member. What has been
        // enabled is the one thing that could join it, and it is off: the node serves standalone.
        // Its sessions are written under its own GUID so that the cluster never reads them, and
        // what the cluster writes does not interfere with what this node is serving.
        const auto nodeId = m_server->getCluster()->nodeId();
        storage->setSessionNamespace(nodeId + ":");
        m_server->logMessage(LogSubject::ServerEvents, LogPriority::Info,
                             format("The storage {} is shared with a cluster, and this node is not a member. "
                                    "Serving standalone under the namespace {}.",
                                    storage->getRedis()->getRedisUrl().toString(), nodeId));

        // A clean start already ran, against the non-namespaced keys. With the namespace now
        // set, the node's sessions - including any stored from a separate standalone run -
        // live under it, so the clean start must reach them too.
        if (!m_settings->m_persistence.m_clean_start.isNull() && m_settings->m_persistence.m_clean_start.asBool())
        {
            RedisConnection::clearNodeState(storage->getRedis(), m_server->getNodeName(),
                                            storage->sessionNamespace());
        }
        return;
    }
}

bool Cluster::clusterExists() const
{
    const auto storage = m_server->getStorage();
    if (!storage || !storage->isPersistent() || !storage->getRedis())
    {
        return false;
    }
    RedisConnect redis;
    redis.connect(storage->getRedis()->getRedisUrl());
    return Coordinator::membership(redis, m_nodeId) >= 0;
}

void Cluster::rejoinInBackground()
{
    // The same thread then keeps the links: whether this node was a member at start or becomes
    // one later, by joining, it has links that can break.
    if (!m_rejoinThread.joinable())
    {
        m_rejoinThread = thread([this]
                                {
                                    rejoin();
                                    keepLinks();
                                });
    }
}

void Cluster::keepLinks()
{
    // A link between two running nodes that broke - closed by the other end, or its CONNECT not
    // answered in time by a node busy with clients' TLS handshakes - was opened again by nothing:
    // links were opened only as a node started, joined or rejoined, and the two nodes stayed
    // unlinked, each one's subscribers out of reach of the other's publishers, until one of them
    // restarted. Once a second, a member that holds its lease and has no link from this node is
    // linked to again.
    constexpr auto checkInterval = 1s;
    constexpr auto step = 100ms;
    while (!m_stopping)
    {
        for (auto waited = 0ms; waited < checkInterval && !m_stopping; waited += step)
        {
            this_thread::sleep_for(step);
        }

        auto* coordinator = m_coordinatorView.load();
        if (m_stopping || coordinator == nullptr || !coordinator->isParticipating())
        {
            continue;
        }
        if (!coordinator->isOnline())
        {
            // Offline, the node has no links: it neither forwards to the nodes it can reach nor
            // takes their traffic as if it were a member. Opening one here would undo the close
            // the state change made, one second later.
            continue;
        }

        vector<Coordinator::Member> members;
        try
        {
            members = coordinator->members();
        }
        catch (const Exception&)
        {
            continue; // The storage is away: there is no telling who is running.
        }

        for (const auto& member: members)
        {
            if (m_stopping)
            {
                return;
            }
            if (member.m_name == m_server->getNodeName() || !member.m_alive || member.m_hostPort.empty())
            {
                continue;
            }
            if (const auto node = findClusterNode(member.m_name); node && node->isConnected())
            {
                continue;
            }

            logMessage(LogPriority::Info, [&member] { return format("No link to node '{}': opening it again.", member.m_name); });
            CServerNode nodeSettings;
            nodeSettings.m_node_name = member.m_name;
            nodeSettings.m_host_port = member.m_hostPort;
            nodeSettings.m_encrypted = true;
            nodeSettings.m_ssl_keys = m_settings->m_connections.m_ssl_keys;
            try
            {
                (void) connectNode(nodeSettings);
            }
            catch (const Exception& e)
            {
                logMessage(LogPriority::Warning, [&member, &e] { return format("Can't link to node '{}': {}", member.m_name, e.what()); });
            }
        }
    }
}

void Cluster::rejoin()
{
    auto* coordinator = m_coordinatorView.load();
    if (coordinator == nullptr)
    {
        return;
    }

    // The members that are running know the rest: a link to one of them is answered with all of
    // them, as a join through it is. Tried in turn, again while none answers.
    while (!m_stopping)
    {
        vector<Coordinator::Member> members;
        try
        {
            members = coordinator->members();
        }
        catch (const Exception& e)
        {
            logMessage(LogPriority::Warning, [&e] { return format("Can't read the cluster members: {}", e.what()); });
        }

        bool anyOther = false;
        for (const auto& member: members)
        {
            if (m_stopping || member.m_name == m_server->getNodeName() || !member.m_alive || member.m_hostPort.empty())
            {
                continue;
            }
            anyOther = true;
            try
            {
                joinCluster(Host(member.m_hostPort));
                if (getConnectedNodeCount(member.m_name) > 0)
                {
                    logMessage(LogPriority::Info, [&member] { return format("Rejoined the cluster through node [{}]", member.m_name); });
                    return;
                }
            }
            catch (const Exception& e)
            {
                logMessage(LogPriority::Warning, [&member, &e]
                           {
                               return format("Can't link to cluster node [{}] at {}: {}", member.m_name, member.m_hostPort, e.what());
                           });
            }
        }
        if (!anyOther)
        {
            // The only member running: there is nobody to link to, and the others link to this
            // node when they start.
            return;
        }
        for (int i = 0; i < 10 && !m_stopping; ++i)
        {
            this_thread::sleep_for(100ms);
        }
    }
}

void Cluster::stop()
{
    m_stopping = true;
    if (m_rejoinThread.joinable())
    {
        m_rejoinThread.join();
    }
    stopSubscriptionSender();
    if (auto* retainedSync = m_retainedSyncView.load())
    {
        retainedSync->stop();
    }
    // A node that stops is down, not gone: it keeps its place among the members, to rejoin with.
    if (auto* coordinator = m_coordinatorView.load())
    {
        coordinator->stop();
    }
    notifyAllNodes(Command::DetachNodeRequest);

    vector<SNode> nodes;
    {
        const scoped_lock lock(m_mutex);
        nodes = m_connectedNodes.nodes();
    }
    for (const auto& node: nodes)
    {
        if (node->getName() != m_server->getNodeName() && node->isConnected())
        {
            node->disconnect();
        }
    }
}

Coordinator& Cluster::coordinatorForCluster()
{
    const scoped_lock lock(m_coordinatorMutex);
    if (!m_coordinator)
    {
        const auto storage = m_server->getStorage();
        if (!storage || !storage->isPersistent() || !storage->getRedis())
        {
            throw Exception("A cluster needs the shared Redis storage: persistence.redis_uri is not set.");
        }
        const auto& leaseSetting = m_settings->m_cluster.m_lease_seconds;
        constexpr int defaultLeaseSeconds = 10;
        const auto    leaseSeconds = leaseSetting.isNull() || leaseSetting.asInteger() <= 0 ? defaultLeaseSeconds
                                                                                            : leaseSetting.asInteger();
        m_coordinator = make_unique<Coordinator>(
            m_nodeId, m_server->getNodeName(), m_settings->m_cluster.m_this_node.m_host_port.asString().c_str(),
            storage->getRedis()->getRedisUrl(), chrono::seconds(leaseSeconds),
            [this](const bool online)
            {
                m_server->onClusterStateChanged(online);
                if (!online)
                {
                    // An offline node is not a member: it neither forwards to the nodes it can reach
                    // nor takes their forwarded traffic as if it were. Closing the links does both,
                    // and keepLinks opens them again once the node holds its lease - it runs on the
                    // rejoin thread and opens what is missing to the members it finds.
                    vector<SNode> nodes;
                    {
                        const shared_lock lock(m_mutex);
                        nodes = m_connectedNodes.nodes();
                    }
                    for (const auto& node: nodes)
                    {
                        node->disconnect();
                    }
                }
                if (online)
                {
                    // Reported only after being offline: what this node held may be another's now,
                    // and the retained messages may have changed without this node hearing of it.
                    queueSessionTask([this] { dropSessionsOwnedElsewhere(); });
                    if (auto* retainedSync = m_retainedSyncView.load())
                    {
                        retainedSync->resynchronize();
                    }
                }
            });
        m_coordinator->onAdoptSessions([this](vector<string> clientIds)
                                       {
                                           queueSessionTask([this, clientIds = std::move(clientIds)] { adoptSessions(clientIds); });
                                       });
        m_coordinatorView = m_coordinator.get();

        m_retainedSync = make_unique<RetainedSync>(
            storage->getRedis()->getRedisUrl(), m_subscriptionManager->retainedMessages(),
            [this](const string& topicName, const RetainedMessages::Record& record) { publishRetained(topicName, record); },
            [this](const string&, const RetainedMessages::Change change) { m_subscriptionManager->countRetainedChange(change); },
            [this](const LogPriority priority, const Logger::OutputString& output) { logMessage(priority, output); });
        m_retainedSyncView = m_retainedSync.get();
    }
    return *m_coordinator;
}

void Cluster::startCoordinator()
{
    coordinatorForCluster().start();
    // After the coordinator, which has made this node a member: the retained messages are read
    // before a client is served.
    m_retainedSyncView.load()->start();
}

size_t Cluster::linkCountOf(const std::string& nodeName) const
{
    const shared_lock lock(m_mutex);
    if (const auto node = m_connectedNodes.findNode(nodeName))
    {
        return node->linkCount();
    }
    return 0;
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

        logMessage(LogPriority::Info,
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
    {
        // Nothing can be sent there any more.
        const scoped_lock lock(m_movedMutex);
        erase_if(m_movedSessions, [&node](const auto& moved) { return moved.second.m_nodeName == node->getName(); });
    }
    logMessage(LogPriority::Info, [&node]
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
            (void) connectNode(nodeSettings);
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
            case AccountsChanged:
                node->publish(make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(AccountsChanged), string_view{}));
                break;
            default:
                break;
        }
    }
}

void Cluster::notifyAccountsChanged() const
{
    notifyAllNodes(Command::AccountsChanged);
}

void Cluster::onAttachNodeRequest(const SPublishMessage& message)
{
    if (!m_settings->m_cluster.m_enabled.isNull() && !m_settings->m_cluster.m_enabled.asBool())
    {
        throw Exception("This node is Standalone. Enable the first node or accept an invitation before attaching.");
    }
    auto attachNodeRequest = deserializeMessage<CCluster>(message);

    if (attachNodeRequest.m_redis_uri.asString() != m_settings->m_persistence.m_redis_uri.asString())
    {
        throw Exception("Cluster nodes must use the same database.");
    }

    if (!attachNodeRequest.m_database_uri.isNull() && attachNodeRequest.m_database_uri.asString() != m_settings->m_authentication.m_database_uri.asString())
    {
        throw Exception("Cluster nodes must share one accounts database.");
    }

    // This node is in a cluster from now on, as its first member if it was alone. The joining node
    // has admitted itself through the storage before asking - unless this request forms the
    // cluster - and one that is not a member, refused there, gets no link back.
    const bool forming = m_coordinatorView.load() == nullptr;
    startCoordinator();
    const string joiningNode = attachNodeRequest.m_this_node.m_node_name.asString().c_str();
    if (!forming && !coordinatorForCluster().isMember(joiningNode))
    {
        logMessage(LogPriority::Error, [&joiningNode]
                   {
                       return format("Node [{}] asked to join, but is not a member of the cluster.", joiningNode);
                   });
        return;
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
    if (!m_settings->m_cluster.m_enabled.isNull() && !m_settings->m_cluster.m_enabled.asBool())
    {
        throw Exception("This node has not accepted an invitation to join the cluster.");
    }
    auto clusterSettings = deserializeMessage<CCluster>(message);

    // Admitted by the node it joined through, so this only starts taking part.
    startCoordinator();

    CCluster attachNodeRequest;
    attachNodeRequest.m_this_node = m_settings->m_cluster.m_this_node;
    attachNodeRequest.m_nodes.push_back(attachNodeRequest.m_this_node);

    for (const auto& nodeSettings: clusterSettings.m_nodes)
    {
        const auto connectedNode = connectNode(nodeSettings);
        if (!connectedNode)
        {
            // This node itself, or one the answering node still lists but that is down: the
            // members link to it when it starts again.
            continue;
        }

        // Don't send the join request to the server that sent the join response:
        if (clusterSettings.m_this_node.m_node_name.asString() != nodeSettings.m_node_name.asString())
        {
            connectedNode->publish(Command::AttachNodeResponse, attachNodeRequest);
        }

        m_connectedNodes.addNode(connectedNode, StoreNodeMode::InsertOnly);
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
        publishMessage->setFromCluster();

        // The shared subscriptions the sending node assigned to this one. Taken off the message:
        // they are for this node, not for the clients it delivers to.
        if (const auto& properties = publishMessage->getProperties())
        {
            vector<string> shares;
            vector<string> sessions;
            properties->forEachUserProperty(
                [&shares, &sessions](const string_view name, const string_view value)
                {
                    if (name == PublishMessage::ClusterShareProperty)
                    {
                        shares.emplace_back(value);
                    }
                    else if (name == PublishMessage::ClusterSessionProperty)
                    {
                        sessions.emplace_back(value);
                    }
                });
            if (!shares.empty())
            {
                properties->removeUserProperty(PublishMessage::ClusterShareProperty);
                publishMessage->setClusterShares(std::move(shares));
            }
            if (!sessions.empty())
            {
                properties->removeUserProperty(PublishMessage::ClusterSessionProperty);
                publishMessage->setClusterSessions(std::move(sessions));
            }
        }
        ++m_forwardedMessagesReceived;
        if (auto* statistics = m_server->systemStatistics())
        {
            statistics->increment(SystemStatistics::SysTopicKind::BrokerClusterMessagesReceived);
        }
        m_server->publishMessage(publishMessage);
    }
}

void Cluster::updateLocalSubscription(const string_view topicFilter, const bool subscribed)
{
    {
        const scoped_lock lock(m_subscriptionMutex);
        if (subscribed)
        {
            m_localSubscriptions.emplace(topicFilter);
        }
        else
        {
            m_localSubscriptions.erase(string(topicFilter));
        }

        // With no other node there is nobody to tell; a node that joins later is sent the whole set.
        if (!m_connectedNodes.anyNodes() || m_stopSubscriptionSender)
        {
            return;
        }

        // Only the change, and not from here: this runs in a client's SUBSCRIBE or session expiry,
        // which must not wait for the network - nor send the node's whole filter set every time,
        // which for N subscriptions is N snapshots of up to N filters each.
        m_pendingSubscriptionChanges.push_back({subscribed, string(topicFilter)});
    }
    m_subscriptionWorkAdded.notify_one();
}

void Cluster::publishRetained(const string& topicName, const RetainedMessages::Record& record) const
{
    if (!m_connectedNodes.anyNodes())
    {
        return;
    }

    Buffer payload;
    RetainedMessages::encode(payload, topicName, record);

    const scoped_lock nodesLock(m_mutex);
    for (const auto& node: m_connectedNodes.nodes())
    {
        if (!node->isConnected() || node->getName() == m_server->getNodeName())
        {
            continue;
        }
        node->publish(retainedMessage(payload));
    }
}

SPublishMessage Cluster::retainedMessage(const Buffer& payload) const
{
    auto message = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::RetainedUpdate),
                                                     string_view(payload.c_str(), payload.bytes()));
    message->setQos(Qos::Qos1);
    return message;
}

void Cluster::publishSubscriptionSnapshot(const SNode& node)
{
    {
        const scoped_lock lock(m_subscriptionMutex);
        if (m_stopSubscriptionSender)
        {
            return;
        }
        m_pendingSnapshots.push_back(node);
    }
    m_subscriptionWorkAdded.notify_one();
}

void Cluster::sendSubscriptions()
{
    // How long a change waits for others to share its message. A burst of subscriptions - a client
    // reconnecting with thousands of them, or thousands of clients - then goes out as a few messages.
    constexpr auto gatherTime = 5ms;

    while (true)
    {
        vector<SubscriptionChange> changes;
        vector<SNode>              snapshotNodes;
        set<string>                snapshot;
        {
            unique_lock lock(m_subscriptionMutex);
            m_subscriptionWorkAdded.wait(lock, [this]
                                         {
                                             return m_stopSubscriptionSender || !m_pendingSubscriptionChanges.empty() ||
                                                    !m_pendingSnapshots.empty();
                                         });
            if (m_stopSubscriptionSender)
            {
                return;
            }
            lock.unlock();
            this_thread::sleep_for(gatherTime);
            lock.lock();

            changes.swap(m_pendingSubscriptionChanges);
            snapshotNodes.swap(m_pendingSnapshots);
            // Taken with the changes, so it already holds every one of them: a node sent this
            // snapshot and then the same changes ends where the set is.
            if (!snapshotNodes.empty())
            {
                snapshot = m_localSubscriptions;
            }
        }

        string snapshotPayload;
        for (const auto& filter: snapshot)
        {
            snapshotPayload.append(filter).push_back('\n');
        }
        for (const auto& node: snapshotNodes)
        {
            if (!node->isConnected())
            {
                continue;
            }
            for (const auto& filter: snapshot)
            {
                node->subscribe(filter);
            }
            auto message = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::SubscriptionSnapshot), snapshotPayload);
            message->setQos(Qos::Qos1);
            node->publish(message);
        }

        if (changes.empty())
        {
            continue;
        }

        string changesPayload;
        for (const auto& [subscribed, filter]: changes)
        {
            changesPayload.push_back(subscribed ? '+' : '-');
            changesPayload.append(filter).push_back('\n');
        }

        vector<SNode> nodes;
        {
            const shared_lock lock(m_mutex);
            nodes = m_connectedNodes.nodes();
        }
        for (const auto& node: nodes)
        {
            if (!node->isConnected() || node->getName() == m_server->getNodeName())
            {
                continue;
            }
            for (const auto& [subscribed, filter]: changes)
            {
                if (subscribed)
                {
                    node->subscribe(filter);
                }
                else
                {
                    node->unsubscribe(filter);
                }
            }
            auto message = make_shared<mqtt::PublishMessage>(m_clusterTopics.getTopic(Command::SubscriptionUpdate), changesPayload);
            message->setQos(Qos::Qos1);
            node->publish(message);
        }
    }
}

namespace {

/// How long a moved session is sent on by name without the node it went to subscribing for it, and
/// how long a message waits for a session that is to arrive: a node that has not subscribed by then
/// has the session no more, and a CONNECT that has not completed by then has failed.
constexpr auto MovedSessionLifetime = 60s;
constexpr auto HeldMessageLifetime = 10s;

} // namespace

vector<pair<string, string>> Cluster::movedSessionsMatching(const string_view topicName)
{
    vector<pair<string, string>> matching;
    const scoped_lock            lock(m_movedMutex);
    if (m_movedSessions.empty())
    {
        return matching;
    }
    const auto now = chrono::steady_clock::now();
    erase_if(m_movedSessions, [now](const auto& moved) { return now - moved.second.m_movedAt > MovedSessionLifetime; });
    for (const auto& [clientId, moved]: m_movedSessions)
    {
        if (ranges::any_of(moved.m_filters, [topicName](const string& filter) { return RetainedMessages::matches(filter, topicName); }))
        {
            matching.emplace_back(moved.m_nodeName, clientId);
        }
    }
    return matching;
}

bool Cluster::divertToMovedSession(const string& clientId, const SMessage& message)
{
    string nodeName;
    {
        const scoped_lock lock(m_movedMutex);
        if (const auto moved = m_movedSessions.find(clientId); moved != m_movedSessions.end())
        {
            nodeName = moved->second.m_nodeName;
        }
    }
    const auto published = dynamic_pointer_cast<mqtt::PublishMessage>(message);
    const auto link = nodeName.empty() || !published ? nullptr : linkFrom(nodeName, published->destination()->fullName());
    if (!link)
    {
        return false;
    }
    auto copy = make_shared<mqtt::PublishMessage>(*published);
    const auto& original = message->getProperties();
    auto        properties = original ? make_shared<MessageProperties>(dynamic_cast<const MessageProperties&>(*original))
                                      : make_shared<MessageProperties>();
    properties->setUserProperty(PublishMessage::ClusterSessionProperty, clientId);
    copy->setProperties(properties);
    link->postMessage(copy, Qos::Qos1, {}, false);
    return true;
}

shared_ptr<ISubscriptionClient> Cluster::linkFrom(const string& nodeName, const string_view topicName) const
{
    // The client ids a node connects to this one with; see ServerNode.
    const auto base = "node_" + nodeName + "_" + m_server->getNodeName();
    const auto first = m_server->getClientSession(base);
    if (!first || first->clusterLinkCount() <= 1)
    {
        return first;
    }
    const auto index = clusterLinkOf(topicName, first->clusterLinkCount());
    return index == 0 ? first : m_server->getClientSession(base + "#" + to_string(index));
}

bool Cluster::holdForSession(const string& clientId, const SPublishMessage& message)
{
    const scoped_lock lock(m_movedMutex);
    const auto        now = chrono::steady_clock::now();
    // Named for as long as the node it left sends on by name, and some.
    erase_if(m_arrivedSessions, [now](const auto& arrived) { return now - arrived.second > 2 * MovedSessionLifetime; });
    if (m_arrivedSessions.contains(clientId))
    {
        return false;
    }
    erase_if(m_heldMessages, [now](auto& held)
             {
                 erase_if(held.second, [now](const HeldMessage& message) { return now - message.m_heldAt > HeldMessageLifetime; });
                 return held.second.empty();
             });
    m_heldMessages[clientId].push_back({message, now});
    return true;
}

void Cluster::sessionArrived(const string& clientId, const shared_ptr<ISubscriptionClient>& session)
{
    vector<HeldMessage> held;
    {
        // Marked arrived with what was held taken, under one lock: a message named for it from now
        // on goes to it directly, and none falls between.
        const scoped_lock lock(m_movedMutex);
        m_arrivedSessions[clientId] = chrono::steady_clock::now();
        if (const auto found = m_heldMessages.find(clientId); found != m_heldMessages.end())
        {
            held = std::move(found->second);
            m_heldMessages.erase(found);
        }
    }
    for (const auto& message: held)
    {
        session->postMessage(message.m_message, message.m_message->getQos(), {}, false);
    }
}

void Cluster::forgetCoveredMoves(const string& nodeName)
{
    set<string> advertised;
    {
        const scoped_lock lock(m_subscriptionMutex);
        if (const auto found = m_nodeSubscriptions.find(nodeName); found != m_nodeSubscriptions.end())
        {
            advertised = found->second;
        }
    }
    // Subscribed for there now: what matches goes there as anything does, and the session is
    // matched by its own subscriptions.
    const scoped_lock lock(m_movedMutex);
    erase_if(m_movedSessions, [&nodeName, &advertised](const auto& moved)
             {
                 return moved.second.m_nodeName == nodeName &&
                        ranges::all_of(moved.second.m_filters, [&advertised](const string& filter) { return advertised.contains(filter); });
             });
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
        case ReleaseSession:
        {
            // "<client id>\n<asking node's GUID>". Let go of off this thread: it waits for Redis.
            const string_view payload = message->payload();
            if (const auto separator = payload.find('\n'); separator != string_view::npos)
            {
                {
                    const scoped_lock lock(m_releaseMutex);
                    m_releaseRequests.emplace_back(string(payload.substr(0, separator)), string(payload.substr(separator + 1)));
                }
                m_releaseAdded.notify_one();
            }
            break;
        }
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
            {
                const scoped_lock lock(m_subscriptionMutex);
                m_nodeSubscriptions[sender] = std::move(filters);
            }
            forgetCoveredMoves(sender);
            break;
        }
        case SubscriptionUpdate:
        {
            // "+filter" or "-filter" per line, in the order the sending node made them.
            {
                const scoped_lock lock(m_subscriptionMutex);
                auto&             filters = m_nodeSubscriptions[sender];
                string_view       payload = message->payload();
                while (!payload.empty())
                {
                    const auto separator = payload.find('\n');
                    const auto line = payload.substr(0, separator);
                    if (line.size() > 1 && line[0] == '+')
                    {
                        filters.emplace(line.substr(1));
                    }
                    else if (line.size() > 1 && line[0] == '-')
                    {
                        filters.erase(string(line.substr(1)));
                    }
                    if (separator == string_view::npos)
                    {
                        break;
                    }
                    payload.remove_prefix(separator + 1);
                }
            }
            forgetCoveredMoves(sender);
            break;
        }
        case RetainedUpdate:
            if (!RetainedMessages::decode(message->payload(),
                                          [this](const string& topicName, const RetainedMessages::Record& record)
                                          {
                                              if (auto* retainedSync = m_retainedSyncView.load())
                                              {
                                                  retainedSync->receive(topicName, record);
                                              }
                                          }))
            {
                logMessage(LogPriority::Error, [&sender]
                           {
                               return format("Damaged retained messages from node '{}'.", sender);
                           });
            }
            break;
        case AccountsChanged:
            m_server->extensions().accountsChanged();
            break;
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

ClusterInvitations& Cluster::invitations()
{
    return m_invitations;
}

const ClusterInvitations& Cluster::invitations() const
{
    return m_invitations;
}

void Cluster::answerHello(const shared_ptr<PublishMessage>& message) const
{
    std::string_view responseTopic;
    const auto properties = message->getProperties();
    if (!properties || !properties->getProperty(Property::ResponseTopic, responseTopic) || responseTopic.empty())
    {
        return;
    }

    // The GUID is the answer, with the name and the version so the inviter can refuse before
    // inviting: a node joins a cluster of its own version.
    xdoc::Document document;
    document.root()->set("node_id", m_nodeId);
    document.root()->set("name", m_server->getNodeName());
    document.root()->set("xmq_version", Server::getVersion());
    document.root()->set("database_uri", m_settings->userDatabaseUri());
    document.root()->set("node_actions", true);
    Buffer payload;
    document.exportTo(xdoc::DataFormat::JSON, payload, false);
    auto answer = make_shared<mqtt::PublishMessage>(m_server->getTopic(responseTopic), String(payload));
    answer->setQos(Qos::Qos1);
    m_server->publishMessage(answer);
}

void Cluster::acceptSharedJoin(const shared_ptr<PublishMessage>& message)
{
    string_view responseTopic;
    const auto properties = message->getProperties();
    if (!properties || !properties->getProperty(Property::ResponseTopic, responseTopic) || responseTopic.empty())
    {
        return;
    }
    String error;
    bool joining = false;
    try
    {
        xdoc::Document request;
        request.load(string(message->payload()));
        const auto root = request.root();
        if (root->getString("node_id") != m_nodeId || root->getString("xmq_version") != Server::getVersion())
        {
            throw Exception("The target UUID or server version does not match the node action request.");
        }
        const auto database = m_settings->userDatabaseUri();
        if (root->getString("database_uri") != database)
        {
            throw Exception("Node actions require both nodes to use the same shared accounts database URI.");
        }
        const auto pool = m_settings->accountsPool();
        if (!pool)
        {
            throw Exception("The shared accounts database is not available.");
        }
        ClusterStore store(pool);
        if (!store.isClusterDatabase())
        {
            throw Exception("Node actions require a shared PostgreSQL or MySQL accounts database.");
        }
        const auto local = store.node(m_nodeId);
        const auto action = root->findFirst("action") ? root->getString("action") : String("join");
        if (action == "disable" || action == "retire")
        {
            if (!local || local->m_state == "Removed" || local->m_state == "Retired")
                throw Exception("This node has no active configuration in the shared database.");
            if (action == "disable" && !m_settings->m_cluster.m_enabled.asBool())
                throw Exception("This node is already Standalone.");
            if (action == "retire" && m_settings->m_cluster.m_enabled.asBool())
                throw Exception("Leave the cluster before retiring this configuration.");
            OSService::queueNodeAction(m_nodeId, database, action);
        }
        else
        {
            if (action != "join") throw Exception("Unsupported selected node action.");
            joining = true;
            const auto leader = store.node(root->getString("leader_id"));
            const auto redisUri = root->getString("redis_uri");
            if (!local || local->m_state == "Removed" || local->m_state == "Retired" || local->m_clusterEnabled)
            {
                throw Exception("This node must have a Standalone configuration before joining.");
            }
            if (!leader || !leader->m_clusterEnabled || leader->m_state == "Removed" ||
                leader->m_state == "Retired" || redisUri.empty() || leader->m_redisUri != redisUri)
            {
                throw Exception("The requested cluster does not match an enabled node in this shared database.");
            }
            RedisConnect redis;
            redis.setReadTimeout(chrono::milliseconds(1000));
            const URL url(redisUri);
            const auto& [host, port] = url.hostAndPort();
            redis.connect(host, port, url.username(), url.password(), url.path(), chrono::milliseconds(1000));
            const auto snapshot = Coordinator::readSnapshot(redis);
            if (ranges::none_of(snapshot.m_members, [&](const auto& member) { return member.m_nodeId == leader->m_nodeId && member.m_alive; }))
            {
                throw Exception("The selected cluster has no live member matching the requesting node.");
            }
            if (snapshot.m_members.size() >= Coordinator::MaxMembers)
            {
                throw Exception("The cluster already has the maximum number of members.");
            }
            error = m_settings->applyInvitationAccept(redisUri, database);
        }
    }
    catch (const exception& exception)
    {
        error = exception.what();
    }
    xdoc::Document response;
    response.root()->set("node_id", m_nodeId);
    response.root()->set("accepted", error.empty());
    response.root()->set("joined", joining && error.empty());
    response.root()->set("error", error);
    Buffer payload;
    response.exportTo(xdoc::DataFormat::JSON, payload, false);
    auto reply = make_shared<mqtt::PublishMessage>(m_server->getTopic(responseTopic), String(payload));
    reply->setQos(Qos::Qos1);
    m_server->publishMessage(reply);
    if (joining && error.empty())
    {
        OSService::queueNodeAction(m_nodeId, m_settings->userDatabaseUri(), "reload");
    }
}

void Cluster::declineInvitation()
{
    const auto invitation = m_invitations.current();
    if (!invitation)
    {
        return;
    }
    m_invitations.decline();

    try
    {
        const Host host(invitation->m_inviterHostPort);
        const auto& settings = m_server->getSettings();
        const auto& keysData = settings->m_connections.m_ssl_keys;
        const auto [nodeCertificate, nodeKey] = Settings::nodeKeyFiles();
        const auto certificateFile = keysData.m_certfile.asString().empty() ? nodeCertificate.string() : keysData.m_certfile.asString();
        const auto privateKeyFile = keysData.m_keyfile.asString().empty() ? nodeKey.string() : keysData.m_keyfile.asString();
        auto authorityFile = keysData.m_cafile.asString();
        if (authorityFile.empty())
        {
            authorityFile = Settings::buildPeerCertificateBundle().string();
        }
        const auto verifyDepth = max<int64_t>(keysData.m_verify_depth.asInteger(), 1);

        client::MqttClient client;
        client.useBrokerThreads();
        const auto rc = client.connect(host, ConnectCredentials(format("xmq-decline-{}", m_nodeId), "cluster", settings->clusterPassword()),
                                       {.m_cleanSession = true}, ProtocolVersion::MqttV5, {},
                                       make_shared<SSLKeys>(privateKeyFile.c_str(), certificateFile.c_str(), "",
                                                            authorityFile.c_str(), SSL_VERIFY_PEER, static_cast<int>(verifyDepth)));
        if (rc == ReasonCode::Success)
        {
            const auto reason = invitation->m_rejectReason.empty() ? String("The administrator declined.") : invitation->m_rejectReason;
            auto message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("cluster/refusal"),
                                                             ClusterInvitations::refusalPayload(m_nodeId, reason));
            message->setQos(Qos::Qos1);
            client.publish(*message);
        }
    }
    catch (const Exception&)
    {
        // The refusal did not arrive; the inviter's invitation expires on its own.
    }
}

void Cluster::onRefusal(const String& payload)
{
    try
    {
        xdoc::Document document;
        document.load(payload);
        const auto nodeIdNode = document.root()->findFirst("node_id");
        const auto reasonNode = document.root()->findFirst("reason");
        if (!nodeIdNode || nodeIdNode->getValue().isNull())
        {
            return;
        }
        const String nodeId = nodeIdNode->getValue().asString();

        if (const auto pool = m_settings->accountsPool())
        {
            ClusterStore store(pool);
            store.createSchema();
            if (const auto row = store.node(nodeId))
            {
                auto refused = *row;
                refused.m_state = "Refused";
                refused.m_lastError = reasonNode && !reasonNode->getValue().isNull() ? reasonNode->getValue().asString() : String();
                store.updateNode(refused);
            }
            store.clearInvitation(nodeId);
        }
    }
    catch (const Exception&)
    {
        // A damaged refusal names no node; nothing to mark.
    }
}

Cluster::InviteResult Cluster::sendInvitation(const String& hostPort, const String& invitedNodeName,
                                               const String& clusterPassword, const bool joinImmediately,
                                               const String& expectedNodeId, const String& nodeAction)
{
    InviteResult result;

    try
    {
        const Host host(hostPort);
        if (host.hostname().empty() || host.port() == 0)
        {
            result.m_error = format("'{}' is not an address a node listens on.", hostPort.c_str());
            return result;
        }

        // The local node's certificate, verified against the peer bundle: an invitation goes only
        // to a node whose certificate is trusted, exactly as a cluster link would.
        const auto& settings = m_server->getSettings();
        const auto& keysData = settings->m_connections.m_ssl_keys;
        const auto [nodeCertificate, nodeKey] = Settings::nodeKeyFiles();
        const auto certificateFile = keysData.m_certfile.asString().empty() ? nodeCertificate.string() : keysData.m_certfile.asString();
        const auto privateKeyFile = keysData.m_keyfile.asString().empty() ? nodeKey.string() : keysData.m_keyfile.asString();
        auto authorityFile = keysData.m_cafile.asString();
        if (authorityFile.empty())
        {
            authorityFile = Settings::buildPeerCertificateBundle().string();
        }
        const auto verifyDepth = max<int64_t>(keysData.m_verify_depth.asInteger(), 1);

        client::MqttClient client;
        client.useBrokerThreads();

        struct ReplyState
        {
            Semaphore ready;
            mutex lock;
            String payload;
            String error;
            bool joining {false};
        };
        const auto reply = make_shared<ReplyState>();
        const auto replyTopic = format("cluster/hello-reply/{}", NodeIdentity::generate());
        client.onMessage([reply](const SPublishMessage& message)
                         {
                             const string_view payload(message->payload());
                             const scoped_lock lock(reply->lock);
                             if (reply->joining && payload.find(R"("accepted")") == string_view::npos &&
                                 payload.find(R"("joined")") == string_view::npos)
                             {
                                 return; // A duplicate hello is not the join acknowledgement.
                             }
                             if (payload.find(R"("node_id")") != string_view::npos)
                             {
                                 reply->payload = string(payload);
                             }
                             else
                             {
                                 reply->error = string(payload);
                             }
                             reply->ready.post();
                         });

        auto properties = make_shared<MessageProperties>();
        properties->setProperty(Property::ResponseTopic, replyTopic);

        // The password the administrator typed into the Invite dialog, or the configured one when
        // there is none to type: the invited node authenticates the cluster user against its own
        // verifier, so presenting the wrong secret is refused here, before anything is sent.
        const auto password = clusterPassword.empty() ? settings->clusterPassword() : clusterPassword;
        const auto rc = client.connect(host, ConnectCredentials(format("xmq-invite-{}", m_nodeId), "cluster", password),
                                       {.m_cleanSession = true}, ProtocolVersion::MqttV5, properties,
                                       make_shared<SSLKeys>(privateKeyFile.c_str(), certificateFile.c_str(), "",
                                                            authorityFile.c_str(), SSL_VERIFY_PEER, static_cast<int>(verifyDepth)));
        if (rc != ReasonCode::Success)
        {
            result.m_error = format("Node '{}' refused the cluster user: {}.", invitedNodeName.c_str(), toString(rc).c_str());
            return result;
        }

        // The reply must be in place before the question goes out.
        client.subscribe(Destination(client::MqttClient::getTopic(replyTopic), SubscriptionOptions(Qos::Qos1)));

        auto hello = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("cluster/hello"), String("{}"));
        hello->setQos(Qos::Qos1);
        hello->setProperties(properties);
        client.publish(*hello);

        if (!reply->ready.wait_for(5s))
        {
            result.m_error = format("Node '{}' did not answer what it is.", invitedNodeName.c_str());
            return result;
        }
        String answerPayload;
        String answerError;
        {
            const scoped_lock lock(reply->lock);
            answerPayload = reply->payload;
            answerError = reply->error;
        }
        if (!answerError.empty())
        {
            result.m_error = answerError;
            return result;
        }

        xdoc::Document document;
        document.load(answerPayload);
        const auto nodeIdNode = document.root()->findFirst("node_id");
        const auto versionNode = document.root()->findFirst("xmq_version");
        if (!nodeIdNode || nodeIdNode->getValue().isNull() || !versionNode || versionNode->getValue().isNull())
        {
            result.m_error = format("Node '{}' answered, but not with an identity.", invitedNodeName.c_str());
            return result;
        }
        const String nodeId = nodeIdNode->getValue().asString();
        const String version = versionNode->getValue().asString();
        if (version != Server::getVersion())
        {
            result.m_error = format("Node '{}' runs XMQ {}, this node {} - a node joins a cluster of its own version.",
                                    invitedNodeName.c_str(), version.c_str(), Server::getVersion().c_str());
            return result;
        }

        if (joinImmediately)
        {
            if (expectedNodeId.empty() || nodeId != expectedNodeId ||
                document.root()->getString("database_uri") != m_settings->userDatabaseUri())
            {
                result.m_error = "The target UUID or shared accounts database does not match the selected configuration.";
                return result;
            }
            const auto capability = document.root()->findFirst("node_actions");
            if (nodeAction != "join" && (!capability || !capability->getValue().asBool()))
            {
                result.m_error = "The selected server must be updated before remote Leave or Retire is available.";
                return result;
            }
            xdoc::Document request;
            request.root()->set("action", nodeAction);
            request.root()->set("node_id", nodeId);
            request.root()->set("leader_id", m_nodeId);
            request.root()->set("xmq_version", Server::getVersion());
            request.root()->set("redis_uri", m_settings->m_persistence.m_redis_uri.asString());
            request.root()->set("database_uri", m_settings->userDatabaseUri());
            Buffer payload;
            request.exportTo(xdoc::DataFormat::JSON, payload, false);
            {
                const scoped_lock lock(reply->lock);
                reply->payload.clear();
                reply->error.clear();
                reply->joining = true;
                while (reply->ready.wait_for(0ms)) {}
            }
            auto join = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic(
                nodeAction == "join" ? "cluster/join" : "cluster/node-action"), String(payload));
            join->setQos(Qos::Qos1);
            join->setProperties(properties);
            client.publish(*join);
            if (!reply->ready.wait_for(5s))
            {
                result.m_error = "The node did not acknowledge the action. Check its state before retrying.";
                return result;
            }
            {
                const scoped_lock lock(reply->lock);
                answerPayload = reply->payload;
                answerError = reply->error;
            }
            if (!answerError.empty())
            {
                result.m_error = answerError;
                return result;
            }
            xdoc::Document response;
            response.load(answerPayload);
            result.m_success = response.root()->getString("node_id") == nodeId && (response.root()->findFirst("accepted") ? response.root()->getBoolean("accepted")
                                                                           : response.root()->getBoolean("joined"));
            if (!result.m_success)
            {
                result.m_error = response.root()->getString("error");
            }
            client.disconnect();
            return result;
        }

        // The node answered who it is: its row, whether the node made it itself or an
        // administrator declared it, is keyed by its GUID from here on. The invitation is the
        // offer, not admission: existing configuration stays unchanged until the target accepts.
        const auto database = m_server->getSettings()->userDatabaseUri();
        const auto redisUri = m_server->getSettings()->m_persistence.m_redis_uri.asString();
        if (const auto pool = m_server->getSettings()->accountsPool(); pool)
        {
            ClusterStore store(pool);
            store.createSchema();

            // A declaration left behind by an earlier Add: the node's real row takes its name, and
            // the database keeps names unique - so the declaration goes, whichever branch is taken.
            if (const auto declared = store.nodeByName(invitedNodeName);
                declared && declared->m_nodeId != nodeId)
            {
                if (!declared->m_certificate.empty() || declared->m_state != "Not Clustered")
                {
                    throw Exception("This name belongs to another server configuration. Remove or rename it explicitly before inviting this node.");
                }
                store.removeNode(declared->m_nodeId);
            }

            if (const auto row = store.node(nodeId))
            {
                if (row->m_state == "Removed" || row->m_state == "Retired")
                {
                    throw Exception("This node's configuration was retired or removed; restore it explicitly before inviting it.");
                }
                auto invited = *row;
                invited.m_state = "Invited";
                // Invite is an offer only. Accept on the target changes its configuration.

                // The name the node answers to now is the real one: an administrator may have
                // declared another, and the row is the node's own.
                invited.m_name = invitedNodeName;
                invited.m_hostPort = hostPort;
                store.updateNode(invited);
            }
            else
            {
                ClusterStore::Node invited;
                invited.m_nodeId = nodeId;
                invited.m_name = invitedNodeName;
                invited.m_hostPort = hostPort;
                invited.m_state = "Invited";
                invited.m_redisUri = redisUri;
                invited.m_databaseUri = database;
                invited.m_clusterEnabled = false;
                store.insertNode(invited);
            }

            ClusterStore::Invitation invitation;
            invitation.m_nodeId = nodeId;
            invitation.m_inviterHostPort = m_settings->m_cluster.m_this_node.m_host_port.asString();
            invitation.m_expiresAt = DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true);
            store.setInvitation(invitation);
        }

        // The invitation itself: what a standalone node has no other way to learn.
        auto inviteProperties = make_shared<MessageProperties>();
        inviteProperties->setUserProperty("origin-node", m_server->getNodeName());
        const auto invitePayload = String(R"({
            "inviter_host_port": ")") + m_settings->m_cluster.m_this_node.m_host_port.asString() + R"(",
            "redis_uri": ")" + redisUri + R"(",
            "database_uri": ")" + database + R"(",
            "xmq_version": ")" + Server::getVersion() + R"("
        })";
        auto invite = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("cluster/invitation"), invitePayload);
        invite->setQos(Qos::Qos1);
        invite->setProperties(inviteProperties);
        client.publish(*invite);
        client.disconnect();

        result.m_success = true;
        return result;
    }
    catch (const Exception& e)
    {
        result.m_error = e.what();
        return result;
    }
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
