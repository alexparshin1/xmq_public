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

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include <sptk5/threads/SynchronizedQueue.h>
#include "ServerData.h"
#include "common/AtomicSharedPtr.h"

#include "Cluster/Cluster.h"
#include "Extensions/ExtensionHost.h"

#include "ClientSession/ClientSession.h"
#include "ClientSession/ClientSessionManager.h"
#include "ClientSession/ClientSessionThreadManager.h"
#include "ControlServiceListener.h"
#include "MessageDeliveryThreads.h"
#include "Settings/Settings.h"
#include "Subscription/SubscriptionManager.h"
#include "Subscription/Subscriptions.h"
#include "SystemStatistics.h"
#include "base/MessageProperties.h"

namespace xmq {

class RedisStorage;

class Bridge;

/**
 * @brief XMQ server class.
 * @remarks Several servers may be created simultaneously, i.e. in the unit tests.
 */
class XMQ_EXPORT Server final : public ServerData
{
    friend class ClientSessionThread;

public:
    /**
     * @brief Constructor.
     * @param settings          Server getSettings.
     * @param logEngine         Log engine.
     * @param startupMessagesLogPriority   Startup messages log priority.
     */
    Server(const std::shared_ptr<Settings>& settings, const std::shared_ptr<sptk::LogEngine>& logEngine, sptk::LogPriority startupMessagesLogPriority = sptk::LogPriority::Info);

    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * @brief Destructor.
     */
    ~Server() override;

    /**
     * @brief Stop the server.
     */
    void stopServer();

    /**
     * @brief Wait until the server is stopped.
     * @param timeout           Timeout.
     * @return true if the server is stopped.
     */
    bool isStopped(std::chrono::milliseconds timeout);

    /**
     * @returns True after shutdown starts. Internal teardown must not publish cluster state.
     */
    [[nodiscard]] bool isStopping() const noexcept
    {
        return static_cast<bool>(m_isStopped);
    }

    /**
     * @return Bridge connections.
     */
    std::shared_ptr<cluster::Cluster> cluster() const
    {
        return m_cluster.load();
    }

    /**
     * @brief Extensions loaded into this broker.
     *
     * Present whether or not anything is configured: with no extensions it costs one atomic load
     * per event site and nothing else.
     */
    [[nodiscard]] ExtensionHost& extensions()
    {
        return m_extensions;
    }

    /**
     * @brief Create a server connection.
     * @remarks Created connection is registered in the server.
     * @param connectionType    Incoming connection type.
     * @param connectionSocket  Incoming connection socket.
     * @param peer              Session peer.
     * @return client connection.
     */
    std::shared_ptr<sptk::ServerConnection> createConnection(sptk::ServerConnection::Type connectionType, SocketType connectionSocket, const sockaddr_in* peer) override;

    ClientSessionThreads getClientSessionThreads();

    void publishMessage(const SPublishMessage& publishMessage) const;

    /**
     * @brief Close client connection.
     * @remarks In takeover mode, the session is not removed from the client session manager.
     * @param clientSession     Client session.
     * @param takeOver          Session takeover flag.
     * @param closingConnection Connection this close is about, when the caller knows it. A
     *                          reconnect can move the session onto a replacement connection at any
     *                          moment, so a caller acting on a particular connection's failure has
     *                          to name it: without it the session's current connection is closed,
     *                          which after a takeover is the socket the reconnect is using.
     *                          Callers with no particular connection in mind - shutdown, detach,
     *                          a takeover close - leave it empty and mean whatever the session
     *                          holds now.
     */
    void closeSession(const SClientSession& clientSession, bool takeOver = false,
                      const std::shared_ptr<ServerConnectionExt>& closingConnection = {});

    /**
     * @brief Close client connection.
     * @remarks In takeover mode, the session is not removed from the client session manager.
     * @param clientId          Client id.
     * @param takeOver          Session takeover flag.
     */
    void closeSession(const std::string& clientId, bool takeOver = false);

    /**
     * @brief Close all client connections.
     */
    void closeConnections();
    void detachSession(const std::string& clientId);

    /**
     * @brief Authenticate client.
     * @param clientId              Client ID.
     * @param username              Username.
     * @param password              Password.
     * @param protocolVersion       Protocol version.
     * @param existingClientSession Existing client session.
     * @return Reason code, or Success.
     */
    /**
     * @brief Decide whether a client may connect, and take over its existing session if it may.
     * @param extensionDecision  What the extensions said, or NotHandled when none was asked.
     */
    ReasonCode authenticate(const std::string& clientId, const std::string& username, const std::string& password,
                            const std::string& remoteAddress, ProtocolVersion protocolVersion,
                            const std::shared_ptr<ClientSession>& existingClientSession,
                            ExtensionHost::AuthDecision           extensionDecision);

    /**
     * @brief Get cluster password.
     * @return Cluster password.
     */
    std::string getClusterPassword() const;

    /**
     * @brief Attach this server to the cluster another node belongs to.
     * @param host              Address of a node already in the cluster.
     * @param encrypted         Must be true; unencrypted cluster connections are rejected.
     */
    void attachToCluster(const sptk::Host& host, bool encrypted = true) const;

    /**
     * @brief Detach from the cluster.
     */
    void detachFromCluster() const;

    /**
     * @brief This node went cluster-online or cluster-offline (see cluster::Coordinator).
     *
     * Offline - it lost the shared storage for longer than its lease - it disconnects every client,
     * whose persistent sessions may be taken over by another node meanwhile, and refuses new ones
     * until it is online again. Cluster links stay up.
     *
     * @param online            True when the node may serve clients again.
     */
    void onClusterStateChanged(bool online);

    /**
     * @brief Let another cluster node take a session over: drop it here and leave its state in Redis.
     *
     * The client, if connected, is disconnected. The session stops receiving here - its
     * subscriptions are withdrawn from this node - and nothing more of it is written to Redis. What
     * it had queued is written first, and kept: those messages are the other node's to deliver now.
     * Returns once Redis holds all of it, so the other node finds the session whole.
     *
     * @param clientId          Client id.
     * @return False if Redis did not confirm the writes in time.
     */
    bool releaseSession(const std::string& clientId);

    /**
     * @brief Grant topic subscription to a client ID.
     * @param client            Client ID.
     * @param topic             Destination topic.
     * @return Reason code or Success.
     */
    static ReasonCode grantSubscription(const SClientSession& client, const Topic* topic);

    /**
     * @brief Grant topic publish to a client ID.
     * @param client            Client session.
     * @param topic             Destination topic.
     * @return Reason code or Success.
     */
    static ReasonCode grantPublish(const SClientSession& client, const Topic* topic);

    /**
     * @brief Subscribe client session to destination.
     * @param client            Client session.
     * @param destination       Subscription destination.
     * @param subscriptionId    Subscription ID.
     * @return subscription.
     */
    std::shared_ptr<Subscription> subscribeClient(const SClientSession& client, const Destination& destination, uint32_t subscriptionId = 0) const;

    /**
     * @brief Unsubscribe the client from destination.
     * @param clientSession     Client session.
     * @param destination       Subscription destination.
     */
    void unsubscribeClient(ClientSession* clientSession, const Destination& destination) const;

    /**
     * @brief Start monitoring incoming data in the client session socket.
     * @param clientSession     Client session.
     * @param rearmOneShot      Re-arm in OneShot socket event trigger mode, otherwise - ignored.
     */
    void watchSession(const SClientSession& clientSession, bool rearmOneShot);

    /**
     * @brief Stop monitoring incoming data in the client session socket.
     * @param clientSession     Client session.
     */
    void unwatchSession(const SClientSession& clientSession);

    /**
     * @brief Set wait for CONNECT message timeout.
     * @return Wait for CONNECT message timeout.
     */
    void setWaitForConnectMessageTimeout(const std::chrono::milliseconds& timeout)
    {
        std::lock_guard lock(m_mutex);
        m_waitForConnectMessageTimeout = timeout;
    }

    /**
     * @brief Get wait for CONNECT message timeout.
     * @return Wait for CONNECT message timeout.
     */
    [[nodiscard]] std::vector<CConnectionInfo> getClientConnectionsInfo(const sptk::RegularExpression& matchClientId) const;

    /**
     * @brief Handle CONNECT message.
     * @param newClientSession     New client session.
     * @param connectMessage       CONNECT message.
     * @return Reason code.
     */
    [[nodiscard]] ReasonCode handleConnectMessage(const SClientSession& newClientSession, const SConnectMessage& connectMessage);

    /**
     * @brief Everything a CONNECT does once it is known whether the client may connect.
     *
     * Split out so that it can run either on the receive thread that read the CONNECT, or later on
     * an authentication thread when an extension was asked and took its time. Nothing in it
     * belongs to a message path, so which of the two runs it does not matter.
     */
    ReasonCode completeConnectMessage(const SClientSession& newClientSession, const SConnectMessage& connectMessage,
                                      ExtensionHost::AuthDecision extensionDecision,
                                      SClientSession*             survivingSession = nullptr);

    /**
     * @brief Get the server version.
     * @return Server version.
     */
    /// Re-reads the extension configuration and applies what can be applied while running.
    ExtensionHost::Report reloadExtensions();

    /// What the Extensions screen shows.
    [[nodiscard]] std::vector<ExtensionHost::Description> describeExtensions() const;

    /// Switches one extension on or off. Enabling re-reads its configuration entry, because it may
    /// have been edited while it was off.
    ExtensionHost::Report switchExtension(const std::string& name, bool on);

    /// Writes an extension's settings to the file its entry came from, then applies them.
    ExtensionHost::Report setExtensionSettings(const std::string&                        name,
                                               const std::map<std::string, std::string>& settings);

    static sptk::String getVersion()
    {
        return Version;
    }

    /**
     * @brief Get cluster object.
     * @return cluster.
     */
    cluster::SCluster getCluster() const
    {
        return m_cluster.load();
    }

    /**
     * @brief Get persistent storage.
     * @return Persistent storage.
     */
    SStorage getStorage() const
    {
        return m_storage;
    }

    /**
     * @brief Whether a published message is matched and delivered on the thread that received it.
     *
     * Not when delivery can block: a persistent store, a bridge or a cluster node is written to from the
     * delivery path, and a receive thread that waits on one stops reading every session it serves.
     * Those messages go through the delivery pool. A bridge starting or stopping while the broker runs
     * switches the path, and one message can then overtake another still queued in the pool.
     */
    [[nodiscard]] bool deliversOnReceiveThread() const noexcept;

    /**
     * @brief Get persistent storage.
     * @return Persistent storage.
     */
    std::shared_ptr<RedisStorage> getRedisStorage() const
    {
        return m_redisStorage;
    }

    /**
     * @brief XMQ web service listener.
     * @return web service listener.
     */


    /**
     * @brief Get all server instances.
     * @return Set of server instances.
     */
    static const std::set<Server*>& instances();

    /**
     * @brief Get system statistics.
     * @return System statistics.
     */
    SystemStatistics* systemStatistics() const
    {
        return m_systemStatistics.get();
    }

    /**
     * @brief Get the subscription manager.
     * @return Subscription manager.
     */
    std::shared_ptr<SubscriptionManager> getSubscriptionManager() const
    {
        return m_subscriptionManager;
    }

    /**
     * @brief Get the asynchronous message delivery thread pool.
     * @return Message delivery threads.
     */
    MessageDeliveryThreads& getMessageDeliveryThreads()
    {
        return m_messageDeliveryThreads;
    }

    /**
     * @brief Accept the cluster message.
     * @param message           Cluster message.
     */
    void acceptClusterMessage(const SPublishMessage& message) const
    {
        const auto& sender = message->getSourceNode();
        m_cluster.load()->acceptClusterMessage(message, sender.empty() ? "incoming" : sender);
    }

    /**
     * @returns session events timer.
     */
    std::shared_ptr<sptk::Timer> getTimer() const
    {
        return m_sessionTimer;
    }

protected:
    void socketEventCallback(const std::shared_ptr<sptk::ServerConnection>& session, sptk::SocketEventType eventType) override; ///< Socket events callback

private:
    mutable std::mutex                   m_mutex;        ///< Mutex that protects internal data.
    std::shared_ptr<sptk::Timer>         m_sessionTimer; ///< Session events timer
    sptk::Flag                           m_isStopped;    ///< True if the server is stopped.
    std::vector<std::shared_ptr<Bridge>> m_bridges;      ///< Outbound bridge connections to other brokers.
    std::atomic<size_t>                  m_bridgeCount {0}; ///< Bridges started and not yet stopped; read without the lock.

    SStorage                          m_storage;                                                 ///< Optionally persistent storage.
    std::shared_ptr<RedisStorage>     m_redisStorage;                                            ///< Redis storage.
    SSubscriptionManager              m_subscriptionManager;                                     ///< Subscription manager.
    ClientSessionThreadManager        m_clientSessionThreads;                                    ///< Message session threads.
    MessageDeliveryThreads            m_messageDeliveryThreads;                                  ///< Message delivery threads.
    std::shared_ptr<sptk::SSLKeys>    m_keys;                                                    ///< Server SSL keys.
    static const sptk::String         Version;                                                   ///< Server version.
    std::chrono::milliseconds         m_waitForConnectMessageTimeout {std::chrono::seconds(30)}; ///< Wait for first CONNECT message timeout.
    SSystemStatistics                 m_systemStatistics;                                        ///< System statistics.
    AtomicSharedPtr<cluster::Cluster> m_cluster;                                                 ///< Cluster connections.
    ExtensionHost                     m_extensions;                                              ///< Loaded extensions; see ExtensionHost.
    /// Where a CONNECT is finished once Redis has answered its session lookup: see
    /// lookUpSessionThen(). A handful is enough - the waiting is Redis's, not theirs.
    static constexpr size_t                          ConnectCompletionThreads = 4;
    sptk::SynchronizedQueue<std::function<void()>>   m_connectCompletions;
    std::vector<std::thread>                         m_connectCompletionThreads;
    std::once_flag                                   m_connectCompletionsStarted;
    std::atomic_bool                                 m_connectCompletionsStopped {false};

    static std::mutex                 m_instancesMutex;                                          ///< Mutex that protects server instances.
    static std::set<Server*>          m_instancesSet;                                            ///< Server instances.

    void clear();

    SClientSession                initializeNewClientSession(const SClientSession& clientSession, const SConnectMessage& connectMessage, bool& loadedPersistentData) const;

    /// A persistent CONNECT from a client this node does not hold yet: its session record has to
    /// come from Redis before the CONNACK can say whether a session is present.
    [[nodiscard]] bool needsSessionLookup(const SConnectMessage& connectMessage) const;

    /**
     * @brief Finish a CONNECT whose session record may have to come from Redis first.
     *
     * The record is asked for asynchronously and no thread waits for it: with Redis syncing every
     * write to disk its answer can take milliseconds, and a thread per waiting CONNECT caps how fast
     * clients can connect at threads divided by that. The CONNECT is finished on a connect
     * completion thread once the answer arrives. Without a lookup to make it is finished here.
     */
    void lookUpSessionThen(const SClientSession& newClientSession, const SConnectMessage& connectMessage,
                           ExtensionHost::AuthDecision decision);

    /// The end of a CONNECT that was left pending: complete it, and read the session again.
    void completePendingConnect(const SClientSession& newClientSession, const SConnectMessage& connectMessage,
                                ExtensionHost::AuthDecision decision);

    void runConnectCompletion(std::function<void()> work);
    void connectCompletionThread();
    void stopConnectCompletions();
    std::shared_ptr<RedisStorage> initializeRedis(const Settings& settings, const std::shared_ptr<sptk::LogEngine>& logEngine);
    SStorage                      initializeStorage(const Settings& settings);
    static void                   handleMqtt5ConnectProperties(const SClientSession& clientSession, const SConnectMessage& connectMessage, ReasonCode& reasonCode);
    SClientSession                takeOverExistingClientSession(const SClientSession& newSession, const SClientSession& existingSession) const;
    sptk::Buffer                  sendConnectAckMessage(const SConnectMessage& connectMessage, ReasonCode reasonCode, const SClientSession& existingClientSession, const SClientSession& clientSession) const;
    void                          createMqttListeners(const std::shared_ptr<Settings>& settings, bool sslKeysLoaded, sptk::LogPriority startupMessagesLogPriority);
    bool                          loadSslKeys(const std::shared_ptr<Settings>& settings);
    void                          restoreServerState() const;

    /**
     * @brief Start the configured outbound bridges.
     *
     * Bridges naming this node are skipped: every node in a deployment normally loads the same
     * bridge list, and a node must not bridge to itself.
     */
    void startBridges();

    /**
     * @brief Stop every running bridge and forget it.
     *
     * Each bridge drops its connection to the remote broker and takes its outbound subscriber
     * out of the local subscriptions, so nothing is left routing to a bridge that no longer
     * exists.
     */
    void stopBridges();

public:
    /**
     * @brief Stop every bridge and start the configured ones again.
     *
     * A bridge is only an outbound MQTT client plus a local subscription, so a configuration
     * change can be applied by rebuilding them rather than restarting the server. Bridges whose
     * configuration did not change are rebuilt too: they reconnect, which is cheap, and it keeps
     * this from having to reason about which entry changed.
     *
     * @remarks Publishing continues throughout; messages that would have crossed a bridge while
     * it is being rebuilt are not carried, exactly as during a reconnect.
     */
    void restartBridges();

    /**
     * @brief Whether every configured outbound bridge has connected.
     *
     * A bridge subscribes on the remote broker only once connected, so nothing published there
     * before that point is carried across. Tests and health checks need to know when a bridge is
     * actually carrying traffic.
     */
    [[nodiscard]] bool bridgesConnected() const;

private:
    void registerNodeTask(const sptk::SRedisConnect& redis);
    void registerNodeUnlocked();
};

using SServer = std::shared_ptr<Server>;

} // namespace xmq
