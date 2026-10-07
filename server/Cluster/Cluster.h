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
#include "../Subscription/RetainedMessages.h"
#include "../Subscription/Subscription.h"

#include "ClusterTopics.h"
#include "Coordinator.h"
#include "ServerNodes.h"
#include "SubscriptionClient.h"
#include "base/xmq.h"

#include <atomic>
#include <condition_variable>
#include <set>
#include <thread>
#include <vector>

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
     * @brief Destructor: stops the subscription sender.
     */
    ~Cluster();

    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    /**
     * @brief Get the cluster time.
     * @return The cluster time.
     */
    sptk::DateTime getClusterTime() const;

    /**
     * @brief Attach this node to the cluster another node belongs to.
     * @param clusterNodeHost   Address of a node already in the cluster.
     * @param encrypted         Must be true: cluster links require TLS and use this node's
     *                          own keys and trust settings. False is rejected.
     * @throws sptk::Exception  When TLS is disabled or the initial connection fails.
     */
    void joinCluster(const sptk::Host& clusterNodeHost, bool encrypted = true);

    /**
     * @brief Take this node's place in the cluster on start, when it has one.
     *
     * Called before the listeners open. A member - by its GUID, in the shared storage - or a node
     * with cluster.enabled starts taking part. A node that is neither, on a database a cluster uses,
     * is refused: its sessions would be written over the cluster's.
     *
     * @throws sptk::Exception  The node may not start: see above, and Coordinator::start().
     */
    void startup();

    /**
     * @brief Link to the other members that are running, found in the storage, in the background.
     *
     * Called once the listeners are open: the members link back to this node.
     */
    void rejoinInBackground();

    /**
     * @return This node's GUID.
     */
    [[nodiscard]] const std::string& nodeId() const
    {
        return m_nodeId;
    }

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
     * @brief Leave the cluster and close every link to the other nodes, before the server stops.
     *
     * The links this node opened keep receiving what the other nodes publish for as long as they
     * are open. Once the server has stopped delivering, a message arriving on one has nowhere to go.
     */
    void stop();

    /**
     * @brief Log a message
     * @param logPriority Log priority
     * @param output Callback returning log message
     */
    void logMessage(sptk::LogPriority logPriority, const sptk::Logger::OutputString& output) const;

    /**
     * @brief Count nodes matching the node name filter.
     * @param nodeNameFilter Regular expression to match node names, or empty string to match all nodes.
     * @return Number of matched nodes.
     */
    size_t   getConnectedNodeCount(const std::string& nodeNameFilter = {}) const;

    /**
     * @brief Check whether the node registry contains any nodes without taking a lock.
     * @return True when the node registry is not empty.
     */
    bool hasNodes() const noexcept
    {
        return m_connectedNodes.anyNodes();
    }
    /**
     * @brief Count registered cluster nodes matching the node name filter.
     * @param nodeNameFilter    Regular expression for node names, or empty to match all nodes.
     * @return Number of matching nodes in the node registry.
     */
    size_t   getClusterNodeCount(const std::string& nodeNameFilter = {}) const;

    /**
     * @brief Get a copy of the cluster configuration.
     * @return Cluster settings, including this node and its known peers.
     */
    CCluster getClusterSettings() const;

    /**
     * @brief Request a connection to a named cluster node.
     * @param nodeName          Name of the node to connect.
     * @note This method is not implemented yet.
     */
    void connectClusterNode(std::string_view nodeName);

    /**
     * @brief Request disconnection from a named cluster node.
     * @param nodeName          Name of the node to disconnect.
     * @note This method is not implemented yet.
     */
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

    /**
     * @brief Update a local effective topic filter and advertise the change to connected peers.
     * @param topicFilter       MQTT topic filter used by local client sessions.
     * @param subscribed        True to add the filter, false to remove it.
     */
    void updateLocalSubscription(std::string_view topicFilter, bool subscribed);

    /**
     * @brief Send a retained message change made on this node to every other node.
     *
     * Every node holds every retained message, whether or not anyone there is subscribed, so this
     * goes to all of them, apart from the subscriptions that route ordinary publications.
     *
     * @param topicName         Topic that changed.
     * @param record            The change, with its time; a tombstone clears.
     */
    void publishRetained(const std::string& topicName, const RetainedMessages::Record& record) const;

    /**
     * @brief Get a copy of a node's effective subscription filters.
     * @param nodeName          Name of the local node or a peer.
     * @return Local filters or the latest peer snapshot; an empty set for an unknown node.
     */
    [[nodiscard]] std::set<std::string> getNodeSubscriptions(std::string_view nodeName) const;

    /**
     * @brief Get the names of nodes in the cluster node registry.
     * @return Registered node names.
     */
    [[nodiscard]] sptk::Strings getClusterNodesNames() const;
    /**
     * @brief Get the server that owns this cluster instance.
     * @return Owning server.
     */
    [[nodiscard]] Server*       getServer() const;

    /**
     * @return True while this node may serve clients: always for a node that is not in a cluster;
     *         for a cluster member, while it holds its client-service lease - while it can reach the
     *         shared storage (see Coordinator).
     */
    [[nodiscard]] bool isOnline() const
    {
        const auto* coordinator = m_coordinatorView.load();
        return coordinator == nullptr || coordinator->isOnline();
    }

    /**
     * @return This node's coordinator, or null while the node is not in a cluster.
     */
    [[nodiscard]] Coordinator* coordinator() const
    {
        return m_coordinatorView.load();
    }

    /**
     * @return How many publications other nodes have forwarded to this one. A node is sent only
     *         what its own clients subscribe to, so with no local subscriber this stays put.
     */
    [[nodiscard]] uint64_t forwardedMessagesReceived() const
    {
        return m_forwardedMessagesReceived.load(std::memory_order_relaxed);
    }

private:
    mutable std::shared_mutex            m_mutex;                  ///< Mutex that protects access to internal data
    mutable std::mutex                   m_subscriptionMutex;      ///< Protects effective subscription snapshots.
    std::map<std::string, std::set<std::string>> m_nodeSubscriptions; ///< Effective filters advertised by each node.
    std::set<std::string>                m_localSubscriptions;      ///< Effective local client filters.

    /// A change of this node's effective filters, waiting to be sent to the other nodes.
    struct SubscriptionChange
    {
        bool        m_subscribed;
        std::string m_filter;
    };
    std::vector<SubscriptionChange> m_pendingSubscriptionChanges; ///< Guarded by m_subscriptionMutex.
    std::vector<SNode>              m_pendingSnapshots;           ///< Nodes owed a full snapshot; guarded by m_subscriptionMutex.
    std::condition_variable         m_subscriptionWorkAdded;      ///< Wakes the subscription sender.
    bool                            m_stopSubscriptionSender {false}; ///< Guarded by m_subscriptionMutex.
    std::thread                     m_subscriptionSender;         ///< Sends snapshots and changes to the other nodes.
    std::atomic<uint64_t>           m_forwardedMessagesReceived {0}; ///< Publications other nodes forwarded here.
    Server*                              m_server;                 ///< Server.
    std::shared_ptr<Settings>            m_settings;               ///< Server settings.
    std::shared_ptr<SubscriptionManager> m_subscriptionManager;    ///< Subscription manager.
    std::shared_ptr<sptk::LogEngine>     m_logEngine;              ///< Log engine.
    SServerNode                          m_thisNode;               ///< This node settings.
    ServerNodes                          m_connectedNodes;         ///< Connected nodes
    Topics                               m_clusterTopics;          ///< Cluster topics.
    SSubscriptionClient                  m_subscriptionClient;     ///< Cluster subscription client.
    sptk::Flag                           m_attachResponseReceived; ///< Flag: Attach request response received.
    std::mutex                           m_coordinatorMutex;       ///< Guards creating m_coordinator.
    std::unique_ptr<Coordinator>         m_coordinator;            ///< Created when the node first joins a cluster.
    std::atomic<Coordinator*>            m_coordinatorView {nullptr}; ///< m_coordinator, read without the lock.
    std::string                          m_nodeId;                 ///< This node's GUID.
    std::thread                          m_rejoinThread;           ///< Links to the other members after startup().
    std::atomic<bool>                    m_stopping {false};       ///< Set by stop(): rejoining gives up.

    /**
     * @brief Link to a member that is running, which links this node to the rest.
     */
    void rejoin();

    /**
     * @return True when the shared storage holds a cluster, with or without this node.
     */
    [[nodiscard]] bool clusterExists() const;

    /**
     * @brief Create the coordinator if there is none yet: the node has just become part of a cluster.
     * @return The coordinator.
     * @throws sptk::Exception  The node has no shared storage: a cluster is not possible without it.
     */
    Coordinator& coordinatorForCluster();

    /**
     * @brief Start taking part in coordination: admit this node, if it is not a member, and run.
     *
     * Called on both sides of a join, and again by every later one; only the first does anything.
     */
    void startCoordinator();

    /**
     * @brief Connect to all nodes on the MQTT level
     */
    void mqttConnectAllNodes();

    /**
     * @brief Establish a peer connection and send the local subscription snapshot.
     * @param nodeSettings      Configuration of the peer to connect.
     * @return Connected peer, or null for this node or a failed connection.
     */
    SNode connectNode(const CServerNode& nodeSettings);
    /**
     * @brief Find a node in the cluster node registry.
     * @param nodeName          Name of the node to find.
     * @return Registered node, or null when no node matches.
     */
    SNode findClusterNode(const std::string& nodeName) const;
    /**
     * @brief Update cluster settings from the current node registry.
     * @note The caller must protect the registry with m_mutex.
     */
    void  updateClusterInfo() const;
    /**
     * @brief Register a connected peer and update the cluster settings.
     * @param node              Peer to register; disconnected peers are ignored.
     */
    void  registerConnectedNode(const SNode& node);
    /**
     * @brief Mark a registered peer offline and update the cluster settings.
     * @param node              Peer that is no longer connected.
     */
    void  deregisterConnectedNode(const SNode& node);
    /**
     * @brief Disconnect a peer and discard its advertised subscription snapshot.
     * @param node              Peer to disconnect and mark offline.
     */
    void  disconnectNode(const SNode& node);
    /**
     * @brief Subscribe to local effective filters on a peer and send their full snapshot.
     * @param node              Connected peer receiving the filters and snapshot.
     */
    void publishSubscriptionSnapshot(const SNode& node);

    /**
     * @brief The subscription sender: sends queued snapshots and changes, in the order they were made.
     *
     * One thread for both, so that a snapshot and the changes after it reach a node in that order:
     * a change sent ahead of an older snapshot would be wiped out by it. It also takes the network
     * out of the path of a client's SUBSCRIBE, and turns a burst of subscriptions into a few
     * messages rather than one per filter.
     */
    void sendSubscriptions();

    /**
     * @brief Stop the subscription sender and wait for it.
     */
    void stopSubscriptionSender();

    /**
     * @brief Send a node that has just been connected every retained record, tombstones included.
     * @param node              Connected node.
     */
    void publishRetainedSnapshot(const SNode& node) const;

    /**
     * @brief A cluster message carrying encoded retained records.
     * @param payload           Records, as RetainedMessages::encode() writes them.
     * @return the message.
     */
    [[nodiscard]] SPublishMessage retainedMessage(const sptk::Buffer& payload) const;

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
    /**
     * @brief Count nodes matching a name filter without acquiring the cluster mutex.
     * @param nodes             Node registry to filter.
     * @param nodeNameFilter    Regular expression for node names, or empty to count the cluster registry.
     * @return Number of matching nodes.
     * @note The caller must protect the registry with m_mutex.
     */
    size_t getNodeCountUnlocked(const ServerNodes& nodes, const std::string& nodeNameFilter) const;

    /**
     * @brief Deserialize a cluster message payload into a configuration object.
     * @tparam T                Object type providing a load method for a document root.
     * @param message           Message containing the serialized object.
     * @return Object loaded from the message payload.
     */
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
