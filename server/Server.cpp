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

#include <sptk5/net/SSLSocket.h>

#include "Bridge/Bridge.h"
#include "ClientSession/ClientSession.h"
#include "SelfSignedCertificate.h"
#include "ServerConnectionExt.h"
#include "base/ReceiveSteering.h"
#include "base/ThreadCount.h"
#include "base/xmq-config.h"
#include "common/ConnectMessage.h"
#include "common/SocketFactory.h"
#include "storage/RedisStorage.h"

#ifdef _WIN32
#include <io.h>
#else
#include <netinet/tcp.h>
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include <fstream>
#include <optional>

using namespace std;
using namespace sptk;
using namespace chrono;
using namespace xmq;

const String Server::Version {XMQ_VERSION_NUMBER};

mutex        Server::m_instancesMutex;
set<Server*> Server::m_instancesSet;
// EdgeTriggered avoids the per-event re-arm epoll_ctl(MOD) on the latency path.
// Windows stays OneShot: wepoll doesn't implement EPOLLET, and SPTK throws on
// EdgeTriggered there.
#ifdef _WIN32
constexpr auto socketEventsMode = SocketPoolTriggerMode::OneShot;
#else
constexpr auto socketEventsMode = SocketPoolTriggerMode::EdgeTriggered;
#endif

std::shared_ptr<RedisStorage> Server::initializeRedis(const Settings& settings, const shared_ptr<LogEngine>& logEngine)
{
    try
    {
        if (!settings.m_persistence.m_enabled.asBool())
        {
            return nullptr;
        }
        // The URL carries the optional username and password through to the Redis connection,
        // so it's passed on whole rather than reduced to host and port.
        URL connectString(settings.m_persistence.m_redis_uri);
        if (const auto [hostname, port] = connectString.hostAndPort();
            hostname.empty())
        {
            connectString = URL("redis", "localhost", 6379,
                                connectString.username(), connectString.password());
        }
        auto maxRedisConnections = settings.m_persistence.m_max_redis_connections.asInteger();
        if (maxRedisConnections < 1)
        {
            maxRedisConnections = 32;
        }
        return make_shared<RedisStorage>(connectString, this, maxRedisConnections, settings.m_persistence.m_clean_start);
    }
    catch (const exception& e)
    {
        const Logger logger(*logEngine, "[Redis Initialization] ");
        logger.error(e.what());
        return nullptr;
    }
}

SStorage Server::initializeStorage(const Settings& settings)
{
    SStorage   storage;
    const auto redisConnectString = settings.m_persistence.m_redis_uri.asString();
    if (const auto storageEnabled = settings.m_persistence.m_enabled.asBool() && !redisConnectString.empty();
        storageEnabled)
    {
        try
        {
            storage = Storage::create(this, redisConnectString, settings.m_persistence.m_clean_start);
            return storage;
        }
        catch (const Exception& exception)
        {
            logMessage(LogSubject::ServerEvents, LogPriority::Error,
                       format("Failed to initialize storage for {}: {}", redisConnectString.c_str(), exception.what()));
        }
    }

    storage = Storage::create(this, "", false);

    return storage;
}

Server::Server(const std::shared_ptr<Settings>& settings, const shared_ptr<LogEngine>& logEngine, const LogPriority startupMessagesLogPriority)
    : ServerData(settings, logEngine, socketEventsMode)
    , m_sessionTimer(make_shared<Timer>())
    , m_storage(initializeStorage(*settings))
    , m_redisStorage(initializeRedis(*settings, logEngine))
    , m_subscriptionManager(make_shared<SubscriptionManager>(this, getTopicManager(), *logEngine))
    , m_clientSessionThreads(this,
                             resolveThreadCount(settings->m_server_limits.m_send_threads.asString()),
                             resolveReceiveThreadCount(settings->m_server_limits.m_receive_threads.asString()),
                             *logEngine)
    , m_messageDeliveryThreads(*m_subscriptionManager,
                               resolveDeliveryThreadCount(settings->m_server_limits.m_delivery_threads.asString()))
    , m_systemStatistics(make_shared<SystemStatistics>(Version, getTopicManager(), m_subscriptionManager))
    , m_cluster(make_shared<cluster::Cluster>(this))
    , m_extensions(*logEngine, getVersion())
{
    if (m_persistentDirectDelivery)
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Warning,
                   "EXPERIMENT: persistent messages are delivered on the receive thread.");
    }

    // The statistics object has no way to reach the thread pools, so the broker hands it the
    // queue depths instead. Called once a second by the metrics scan, never from the message path:
    // reading a queue's size takes the same mutex its push and pop take.
    m_systemStatistics->setQueueDepthProvider(
        [this]
        {
            return SystemStatistics::QueueDepths {.receive = m_clientSessionThreads.receiveQueueLength(),
                                                  .send = m_clientSessionThreads.sendQueueLength(),
                                                  .delivery = m_messageDeliveryThreads.queueLength()};
        });

    bool sslKeysLoaded;
    try
    {
        sslKeysLoaded = loadSslKeys(settings);
    }
    catch (const exception& e)
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Error, [message = e.what()]
                   {
                       return "Can't load SSL keys: " + string(message) + ".";
                   });
        sslKeysLoaded = false;
    }

    // Which subjects produce events, decided before anything can raise one. By subject, not by
    // event type, because an error is an event of whichever subject it happened in - so one switch
    // governs both what a subject does and what it fails to do, and an installation turns events
    // off with the same names it already uses for the log.
    //
    // Every subject, not only the five with an event type of their own: an error can be raised in
    // any of them.
    uint32_t enabledEvents = 0;
    Strings  disabledSubjects;
    for (auto value = 0U; value <= static_cast<unsigned>(LogSubject::StorageEvents); ++value)
    {
        const auto subject = static_cast<LogSubject>(value);
        if (settings->eventEnabled(subject))
        {
            enabledEvents |= ExtensionHost::subjectBit(subject);
        }
        else
        {
            disabledSubjects.push_back(to_string(subject));
        }
    }
    m_extensions.enableEvents(enabledEvents);

    // Before start(), because an authenticator asks for it there: an extension that answers out of
    // the broker's own accounts is told where they are rather than configured with the address a
    // second time. Two addresses, set in two places, is how the interface and the authenticator
    // end up on different databases with nothing to notice it.
    m_extensions.userDatabaseUri(settings->userDatabaseUri());

    // After the broker exists and before it listens: an extension may want to be running when the
    // first client arrives, and none of them may keep the broker from starting.
    m_extensions.start(ExtensionHost::readConfiguration(settings->configurationPath(), *logEngine));

    // A broker with no authenticator admits no named client at all, because there is no longer a
    // fallback to its own accounts. That is the intended shape - authentication belongs to an
    // extension - but it is indistinguishable from a broken broker unless it is said, and the log
    // at start-up is the only place anybody looks before the first refused CONNECT.
    if (!m_extensions.authenticating())
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Error, [&settings]
                   {
                       return settings->userManager().isAllowAnonymous()
                                  ? "No extension authenticates, so only anonymous clients can connect. "
                                    "Configure an authenticator in xmq_extensions.d."
                                  : "No extension authenticates and anonymous connections are not allowed, so "
                                    "nobody can connect. Configure an authenticator in xmq_extensions.d.";
                   });
    }

    // Said once, and only when somebody is watching: an observer that receives nothing looks
    // exactly like one that works, and the reason is in a configuration file rather than in the
    // extension. Publish is off unless asked for, which is the case most likely to puzzle whoever
    // wrote an extension that counts messages.
    if (m_extensions.watching() && !disabledSubjects.empty())
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Info, [&disabledSubjects]
                   {
                       return "Extension events are switched off for: " + disabledSubjects.join(", ") +
                              ". See 'events' in the configuration.";
                   });
    }

    logMessage(LogSubject::ServerEvents, startupMessagesLogPriority, []
               {
                   return "XMQ server version " + getVersion() + ".";
               });

    if (m_redisStorage)
    {
        try
        {
            logMessage(LogSubject::ServerEvents,
                       LogPriority::Info,
                       format("Using Redis at {}.", m_redisStorage->host().toString()));
            m_redisStorage->connect();
        }
        catch (const exception& e)
        {
            logMessage(LogSubject::ServerEvents, LogPriority::Error, [message = e.what()]
                       {
                           return "Can't connect to Redis: " + string(message) + ".";
                       });
        }
    }

    // Restore before any listener exists. The restore rebuilds subscriptions, and a
    // subscription that has not been rebuilt yet cannot be matched: a message published
    // in that window is silently dropped for the sessions still queued for restore.
    // Reconnecting clients are safe either way - ClientSession::factory() loads a session
    // from Redis on demand - but publishers are not, so the port must stay closed until
    // the whole state is back.
    const auto redisForRestore = m_storage ? m_storage->getRedis() : nullptr;
    logMessage(LogSubject::ServerEvents, startupMessagesLogPriority,
               [storage = redisForRestore ? redisForRestore->toString() : string("memory")]
               {
                   return format("Using storage at {}.", storage);
               });
    restoreServerState();

    createMqttListeners(settings, sslKeysLoaded, startupMessagesLogPriority);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);

    constexpr auto maxRandom = 0xFFFFFFFF;
    srandom(static_cast<unsigned>(chrono::duration_cast<seconds>(steady_clock::now().time_since_epoch()).count() % maxRandom));
#endif

    if (m_waitForConnectMessageTimeout.count() == 0)
    {
        m_waitForConnectMessageTimeout = 10s;
    }

    cluster::Cluster::connectToCluster();

    FastTCPServer::start();

    // After the server is started, not before: a bridge connects out as an MQTT client and
    // immediately sends its SUBSCRIBE. Started earlier, the connection is accepted by the remote
    // but everything written after the CONNECT is lost, leaving a bridge that logs itself as
    // connected and carries nothing until it happens to reconnect.
    startBridges();

    logMessage(LogSubject::ServerEvents, startupMessagesLogPriority, "Server started.");

    // Register server instance
    scoped_lock lock(m_instancesMutex);
    m_instancesSet.insert(this);
}

void Server::registerNodeTask(const SRedisConnect&)
{
    try
    {
        // registerNodeQuery->param("node_name") = getNodeName();
        // registerNodeQuery->param("node_state") = static_cast<int>(getNodeState());
        // registerNodeQuery->param("settings") = getSettings()->toString();
        // registerNodeQuery->param("connected_nodes") = "";
        // const auto nodeId = registerNodeQuery->scalar().asInteger();
        // setNodeId(nodeId);
    }
    catch (const Exception& e)
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Error, format("Can't register node {}: {}", getNodeName(), e.what()));
    }
}

void Server::registerNodeUnlocked()
{
    if (!m_storage || !m_storage->isPersistent()) { return; }

    // if (conn)
    // {
    //     registerNodeTask(conn);
    // }
    // completed.post();
}

void Server::startBridges()
{
    const auto& settings = getSettings();
    if (!settings)
    {
        return;
    }

    for (const auto& bridgeSettings: settings->m_bridges)
    {
        if (bridgeSettings.m_enabled.isNull() || !bridgeSettings.m_enabled.asBool())
        {
            continue;
        }

        // "cluster" entries are handled by the cluster, not here. Every other configured mode is
        // started, "out" included: an outbound-only bridge subscribes locally and forwards, which
        // Bridge::subscribeOutboundTopics() implements. Leaving it out of this list made such a
        // bridge silently never exist - no connection, no error, and nothing in the log.
        if (const auto mode = bridgeSettings.m_mode.asString();
            mode != "in" && mode != "out" && mode != "inout")
        {
            continue;
        }

        // Every node normally loads the same bridge list, so the entry describing this node is
        // the one it must not act on.
        if (bridgeSettings.m_node_name.asString() == getNodeName())
        {
            continue;
        }

        // Counted before it starts: it can forward as soon as it does.
        m_bridgeCount.fetch_add(1, memory_order_relaxed);
        auto bridge = make_shared<Bridge>(this, bridgeSettings);
        bridge->start();

        const scoped_lock lock(m_mutex);
        m_bridges.push_back(std::move(bridge));
    }
}

void Server::stopBridges()
{
    // Taken out of the member first, so the stopping - which joins each bridge's thread, and can
    // take a moment for one that is mid-connect - happens outside the server mutex. A bridge
    // thread reaches back into the server, so holding it here would invite a deadlock.
    vector<shared_ptr<Bridge>> bridges;
    {
        const scoped_lock lock(m_mutex);
        bridges.swap(m_bridges);
    }

    for (const auto& bridge: bridges)
    {
        bridge->stop();
    }
    m_bridgeCount.fetch_sub(bridges.size(), memory_order_relaxed);
}

bool Server::deliversOnReceiveThread() const noexcept
{
    if ((m_storage && m_storage->isPersistent() && !m_persistentDirectDelivery) ||
        m_bridgeCount.load(memory_order_relaxed) != 0)
    {
        return false;
    }
    const auto cluster = getCluster();
    return !cluster || !cluster->hasNodes();
}

void Server::restartBridges()
{
    logMessage(LogSubject::ServerConnections, LogPriority::Info, "Restarting bridges.");
    stopBridges();
    startBridges();
}

bool Server::bridgesConnected() const
{
    const scoped_lock lock(m_mutex);
    return std::ranges::all_of(m_bridges, [](const auto& bridge)
                               {
                                   return bridge->isConnected();
                               });
}

void Server::restoreServerState() const
{
    if (m_storage->isPersistent())
    {
        logMessage(LogSubject::ServerEvents, LogPriority::Info, "Restore server state started.");

        // Retained messages first: a session restored below may hold a subscription, and the
        // retained message matching it has to be there by the time anything can be delivered.
        getSubscriptionManager()->retainedMessages().load(m_storage, getNodeName());

        // The counter has to be told what was just restored. Left at zero, the first client to
        // clear a restored retained message makes the decrement throw, and the throw happens
        // before the store is updated - so the cleared message comes back on the next restart.
        if (auto* statistics = systemStatistics(); statistics != nullptr)
        {
            statistics->setValue(SystemStatistics::SysTopicKind::BrokerMessagesRetainedCount,
                                 getSubscriptionManager()->retainedMessages().size());
        }

        getClientSessionManager()->load(m_storage, getNodeName());
        logMessage(LogSubject::ServerEvents, LogPriority::Info, "Restore server state completed.");
    }
}

bool Server::loadSslKeys(const shared_ptr<Settings>& settings)
{
    logMessage(LogSubject::ServerEvents, LogPriority::Debug, []
               {
                   return "Initializing SSL keys..";
               });
    auto sslKeysLoaded = false;
    if (const auto settingsKeys = settings->m_connections.m_ssl_keys;
        !settingsKeys.m_certfile.asString().empty())
    {
        auto verifyDepth = settingsKeys.m_verify_depth.asInteger();
        auto verifyMode = verifyDepth ? SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE : SSL_VERIFY_NONE;
        try
        {
            // Issued here when there is nothing at those paths, the same way the configuration
            // interface gets its own. An installation that was never given a certificate still
            // serves MQTT+SSL, and does it with a key of its own rather than one every other
            // installation of XMQ also holds. A pair already in place is left untouched.
            if (String description;
                SelfSignedCertificate::create(settingsKeys.m_certfile.asString().c_str(),
                                              settingsKeys.m_keyfile.asString().c_str(),
                                              settings->nodeHostName(), description))
            {
                logMessage(LogSubject::ServerEvents, LogPriority::Info, "Created a " + description);
            }

            m_keys = make_shared<SSLKeys>(settingsKeys.m_keyfile.asString().c_str(),
                                          settingsKeys.m_certfile.asString().c_str(), "",
                                          settingsKeys.m_cafile.asString().c_str(), verifyMode, verifyDepth);
            if (m_keys)
            {
                setSSLKeys(m_keys);
                sslKeysLoaded = true;
            }
        }
        catch (const Exception& exception)
        {
            logMessage(LogSubject::ServerEvents, LogPriority::Error,
                       format("Initializing SSL keys: {}", exception.message().c_str()));
        }
    }
    return sslKeysLoaded;
}

namespace {

/**
 * @brief The kernel's own ceiling on the listen() backlog, where it can be read.
 *
 * listen() takes the backlog as a request, not an instruction: the kernel silently clamps it to a
 * system-wide maximum. When that maximum is the smaller number, a burst of connections overflows
 * the accept queue and is refused *by the kernel*, before this process is involved at all - no
 * failed accept(), no error, nothing the broker can observe. Reading the ceiling at start-up is
 * therefore the only chance to say anything about it.
 */
optional<int> listenBacklogLimit()
{
#if defined(_WIN32)
    // Nothing to query: Windows reads SOMAXCONN as "choose a reasonable maximum" and does not
    // publish what it chose.
    return {};
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    int    limit = 0;
    size_t length = sizeof(limit);
    if (sysctlbyname("kern.ipc.somaxconn", &limit, &length, nullptr, 0) != 0 || limit <= 0)
    {
        return {};
    }
    return limit;
#else
    ifstream somaxconn("/proc/sys/net/core/somaxconn");
    int      limit = 0;
    if (!somaxconn || !(somaxconn >> limit) || limit <= 0)
    {
        return {};
    }
    return limit;
#endif
}

/// The name an administrator would raise, and what a client sees when the queue overflows. The two
/// go together: the symptom is what somebody arrives with, and it does not name the cause on either
/// platform - FreeBSD answers the overflowing connection with an RST, which reaches the client as
/// "connection refused", exactly as if nothing were listening on the port at all, while Linux drops
/// the SYN and leaves the client to time out.
struct ListenBacklogAdvice
{
    string_view setting;
    string_view persistIn;
    string_view symptom;
};

constexpr ListenBacklogAdvice listenBacklogAdvice()
{
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    return {.setting = "kern.ipc.somaxconn",
            .persistIn = "/etc/sysctl.conf",
            .symptom = "clients are refused as if nothing were listening on the port"};
#else
    return {.setting = "net.core.somaxconn",
            .persistIn = "/etc/sysctl.d/",
            .symptom = "clients time out with no answer at all"};
#endif
}

/// Where the user manual explains receive packet steering: when it helps and how to turn it on.
constexpr string_view receiveSteeringDocumentation = "https://xmq.sptk.net/xmq_documentation#receive-steering";

} // namespace

void Server::createMqttListeners(const shared_ptr<Settings>& settings, const bool sslKeysLoaded, LogPriority startupMessagesLogPriority)
{
    // Once, before any port opens, and only when the kernel is the one imposing the smaller number.
    if (const auto limit = listenBacklogLimit();
        limit.has_value() && *limit < DEFAULT_LISTEN_BACKLOG)
    {
        constexpr auto advice = listenBacklogAdvice();
        logMessage(LogSubject::ServerEvents, LogPriority::Warning,
                   format("Listen backlog is capped at {} by {}, below the {} this broker asks for. "
                          "Connections arriving faster than they are accepted are then discarded by "
                          "the kernel before the broker sees them - it cannot report them, and {}. "
                          "Raise it with 'sysctl -w {}={}', and keep it in {}.",
                          *limit, advice.setting, DEFAULT_LISTEN_BACKLOG, advice.symptom,
                          advice.setting, DEFAULT_LISTEN_BACKLOG, advice.persistIn));
    }

    // Also once, and also something only the host can fix. A network card with one receive queue,
    // and nothing steering it, runs the receive path of every client on a single CPU. That CPU is
    // the ceiling nobody looks at: it saturates in softirq while the broker's own threads have room,
    // the card drops frames, and clients in retransmit backoff go silent long enough to be closed
    // for keep-alive - which reads as a broker fault. On the bench it was 255 891 frames a second
    // and a 100k msg/s scenario that never finished. Only the enabled listeners' addresses count.
    {
        vector<string> bindAddresses;
        for (const auto& listener: settings->m_connections.m_listener)
        {
            if (listener.m_protocol.asString().empty() || listener.m_port.asInteger() == 0 ||
                (!listener.m_enable.isNull() && !listener.m_enable.asBool()))
            {
                continue;
            }
            bindAddresses.push_back(listener.m_bind_ip.asString());
        }

        for (const auto& gap: ReceiveSteering::findGaps(bindAddresses, thread::hardware_concurrency()))
        {
            logMessage(LogSubject::ServerEvents, LogPriority::Warning,
                       format("Network interface {} has a single receive queue and no receive packet steering, "
                              "so the kernel processes every client's packets on one CPU. Under heavy load that "
                              "CPU saturates, the card drops frames, and clients are disconnected for keep-alive "
                              "timeouts although they are sending. Consider enabling RPS: {}",
                              gap.interfaceName, receiveSteeringDocumentation));
        }
    }

    for (const auto& listener: settings->m_connections.m_listener)
    {
        if (listener.m_protocol.asString().empty() || listener.m_port.asInteger() == 0)
        {
            continue;
        }

        // A listener the configuration disables must not open its port.
        if (!listener.m_enable.isNull() && !listener.m_enable.asBool())
        {
            stringstream disabledMessage;
            disabledMessage << "Listener " << listener.m_name.asString()
                            << " on port " << listener.m_port.asInteger() << " is disabled.";
            logMessage(LogSubject::ServerEvents, startupMessagesLogPriority, disabledMessage.str());
            continue;
        }

        stringstream message;
        message << "Listening " << listener.m_protocol.asString().toUpperCase()
                << " on port " << listener.m_port.asInteger()
                << " (" + listener.m_threads.asString() + " thread";
        if (listener.m_threads.asInteger() > 1)
        {
            message << "s";
        }
        message << ").";
        logMessage(LogSubject::ServerEvents, startupMessagesLogPriority, message.str());
        const auto connectionType = listener.m_protocol.asString().toUpperCase() == "MQTT" ? ServerConnection::Type::TCP : ServerConnection::Type::SSL;
        if (!sslKeysLoaded && connectionType == ServerConnection::Type::SSL)
        {
            logMessage(LogSubject::ServerEvents, LogPriority::Error, "MQTT+SSL listener is not created.");
        }
        else
        {
            const auto bindIP = listener.m_bind_ip.asString().empty() ? String("0.0.0.0") : listener.m_bind_ip.asString();
            Host       host {bindIP, static_cast<uint16_t>(listener.m_port.asInteger())};
            addListener(connectionType, host, static_cast<uint16_t>(listener.m_threads.asInteger()));
        }
    }
}

Server::~Server()
{
    try
    {
        stopServer();
    }
    catch (const Exception& e)
    {
        CERR("Server::stopServer() failed: " << e.what());
    }
}

void Server::stopServer()
{
    if (!m_isStopped)
    {
        m_isStopped = true;

        m_sessionTimer.reset();

        m_messageDeliveryThreads.terminateThreads();

        m_clientSessionThreads.stop();

        m_cluster.load()->detachCluster();

        FastTCPServer::stop();

        // After the listeners, so nothing is still raising events into a host that is unloading
        // the libraries the handlers live in.
        m_extensions.stop();

        if (m_storage)
        {
            m_storage->disconnect();
        }

        closeConnections();

        clear();

        if (m_systemStatistics)
        {
            m_systemStatistics.reset();
        }

        logMessage(LogSubject::ServerEvents, LogPriority::Info,
                   []
                   {
                       return "Server stopped.";
                   });

        scoped_lock lock(m_instancesMutex);
        m_instancesSet.erase(this);
    }
}

bool Server::isStopped(const milliseconds timeout)
{
    return m_isStopped.wait_for(true, timeout);
}

shared_ptr<ServerConnection> Server::createConnection(const ServerConnection::Type connectionType, const SocketType connectionSocket, const sockaddr_in* peer)
{
    try
    {
        logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                   [peer]
                   {
                       auto [address, port] = ServerConnection::parseAddress(peer);
                       return format("Connection from {}:{}.", address, port);
                   });

        const STCPSocket socket = createConnectionSocket(connectionType, connectionSocket);

        auto connection = std::make_shared<ServerConnectionExt>(connectionType, peer);
        connection->setSocket(socket);

        const auto newSession = ClientSession::factory(this);
        newSession->setConnection(connection, *getGenericProtocols());

        newSession->closeSessionIfNoConnectMessageAfter(m_waitForConnectMessageTimeout);
        return connection;
    }
    catch (const Exception& exception)
    {
        logMessage(LogSubject::ServerConnections, LogPriority::Error,
                   [&exception]
                   {
                       return exception.what();
                   });

#ifdef _WIN32
        _close(static_cast<int>(connectionSocket));
#else
        close(connectionSocket);
#endif
    }

    return nullptr;
}

void Server::closeConnections()
{
    FastTCPServer::closeAllConnections();

    m_storage->disconnect();
    getClientSessionManager()->clear();

    FastTCPServer::stop();
}

void Server::detachSession(const string& clientId)
{
    if (const auto clientSession = getClientSessionManager()->find(clientId))
    {
        clientSession->unsubscribeAll();
        closeSession(clientSession, false);
    }
}

void Server::closeSession(const string& clientId, const bool takeOver)
{
    if (const auto clientSession = getClientSessionManager()->find(clientId))
    {
        closeSession(clientSession, takeOver);
    }
}

void Server::closeSession(const SClientSession& clientSession, const bool takeOver,
                          const shared_ptr<ServerConnectionExt>& closingConnection)
{
    if (closingConnection && clientSession->getConnection() != closingConnection)
    {
        // Overtaken: the connection whose failure brought us here is not the one the session is
        // serving any more, so nothing below is about this session. Unwatching or closing here
        // would act on the replacement - the socket the reconnect is using - and the client would
        // be left holding a connection the broker never reads again, its CONNECT unanswered.
        //
        // Asking the session which connection to close cannot catch this: by the time the question
        // is put, the takeover has already answered it with the replacement. Only the caller knows
        // which connection the event was for.
        if (closingConnection->claimForClose())
        {
            closeConnection(closingConnection);
        }
        return;
    }

    if (clientSession->isConnected())
    {
        // The connection this close is about, for callers that had no particular one in mind.
        // Nothing holds a lock across the steps below, so a reconnect can hand the session a
        // different connection between them, and ClientSession::closeSession() compares against
        // this one before dropping anything.
        const auto connection = closingConnection ? closingConnection : clientSession->getConnection();

        // One thread takes this connection down, and the rest leave it alone. Two closes for the
        // same connection arrive routinely - the reactor sees the hangup, a worker sees its write
        // fail - and the steps below are not atomic between calls: the second would reach
        // unwatchConnection() while the first was already closing the descriptor. See
        // ServerConnectionExt::claimForClose() for what that costs.
        if (connection && !connection->claimForClose())
        {
            return;
        }

        // Named, not asked for again. unwatchSession() would take the session's connection as it
        // stands at this instant, and a takeover between the check above and here would make that
        // the replacement - leaving the reconnect's socket out of the reactor, so the SUBSCRIBE
        // that follows its CONNECT is never read and never answered.
        if (connection)
        {
            unwatchConnection(connection);
        }

        // The connection check lives inside closeSession(), under the session's own lock, so that
        // an overtaken close cannot drop the replacement's socket. The check above is the same one,
        // made earlier and without that lock, and a takeover can land between the two - so this is
        // the answer that counts.
        //
        // The teardown below used to run either way. When the close had been overtaken it then
        // took a live session out of the manager: the broker had answered a CONNECT and forgotten
        // the client in the same breath, so the client believed itself connected and its SUBSCRIBE
        // was never answered. That is the shape a bridge shows when it reconnects and then carries
        // nothing at all.
        const auto closed = clientSession->closeSession(connection);

        const auto& clientId = clientSession->getClientId();

        if (closed && clientSession->isCleanSession())
        {
            if (!takeOver)
            {
                getClientSessionManager()->remove(clientSession);
            }
            clientSession->clearProtocol();
            clientSession->clearSession();
        }

        if (systemStatistics())
        {
            systemStatistics()->registerDisconnectedClient(clientSession->isCleanSession());
        }

        logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                   [&clientId]
                   {
                       return clientId + " disconnected.";
                   });
    }
}

void Server::socketEventCallback(const shared_ptr<ServerConnection>& connection, const SocketEventType eventType)
{
    const auto connectionExt = dynamic_pointer_cast<ServerConnectionExt>(connection);
    const auto session = connectionExt->getClientSession();

    if (!session)
    {
        // A connection that has been superseded by a reconnect, or is otherwise no longer bound to
        // a session. Whatever it reports now says nothing about the session that replaced it - but
        // it is still ours to release: nothing else holds it, so leaving it here keeps its socket
        // open and its entry in the reactor for the lifetime of the server.
        closeConnection(connectionExt);
        return;
    }

    if (eventType.m_hangup || eventType.m_error)
    {
        // Recorded against the connection the event is for, not against the session. If the client
        // has already reconnected, the session is serving a different connection and must not be
        // torn down on the strength of this one ending - doing so closes the socket the reconnect
        // is using, and nothing the client sends on it is ever read.
        connectionExt->setHangup(true);
        if (!eventType.m_data)
        {
            // Named, not left to be looked up: between this event and the close, a reconnect may
            // hand the session a new connection, and a close that asks the session which connection
            // it holds would then find - and drop - the replacement.
            closeSession(session, false, connectionExt);
            return;
        }
    }

    if (eventType.m_data)
    {
        if (LatencyTrace::tracingEnabled())
        {
            session->setReactorReadyTimestamp(LatencyTrace::now());
        }
        session->clientSessionReceiveThread()->queueProcessSession(session);
    }
}

SClientSession Server::initializeNewClientSession(const SClientSession& clientSession, const SConnectMessage& connectMessage, bool& loadedPersistentData) const
{
    clientSession->setHangup(false);
    clientSession->clearSession(connectMessage);
    loadedPersistentData = clientSession->load();

    {
        const std::scoped_lock lock(m_mutex);
        getClientSessionManager()->add(clientSession);
    }
    // Only ordinary clients are announced. A cluster session is the link between two nodes, and
    // every node names its own end the same way, so announcing one asks the other nodes to tear
    // down their own links. The receiver used to guess this from the client id starting with
    // "node", which both missed the SubscriptionClient's id of "cluster" and exempted any real
    // client whose id happened to begin with those four letters.
    if (!clientSession->isClusterSession())
    {
        m_cluster.load()->notifyAllNodes(cluster::Command::DisconnectClient, clientSession->getClientId());
    }

    return clientSession;
}

SClientSession Server::takeOverExistingClientSession(const SClientSession& newSession, const SClientSession& existingSession) const
{
    const std::scoped_lock lock(m_mutex);

    existingSession->setHangup(false);
    existingSession->setConnection(newSession->getConnection(), *getGenericProtocols());
    newSession->clearConnection();

    return existingSession;
}

namespace {

/**
 * @brief Everything the CONNACK tells a client about this server, built in one place.
 *
 * One place, because there used to be two: these values, and then a loop that copied the client's
 * own CONNECT properties over the top of them. Every one of these fields describes the server -
 * how many unacknowledged publications it will take, how large a packet it will accept, how high a
 * topic alias it understands - so a value that came from the client is never the right answer.
 */
SMessageProperties connectAckProperties(const Settings& settings)
{
    using enum Property;

    auto properties = make_shared<MessageProperties>();
    properties->setProperty(ReceiveMaximum, settings.m_queue_limits.m_max_inflight_messages.asInteger());
    properties->setProperty(MaximumPacketSize, settings.m_server_limits.m_max_packet_size.asInteger());
    properties->setProperty(TopicAliasMaximum, settings.m_server_limits.m_max_topic_alias.asInteger());
    properties->setProperty(RetainAvailable, 1);
    properties->setProperty(WildcardSubscriptionAvailable, 1);
    properties->setProperty(SubscriptionIdentifierAvailable, 1);
    return properties;
}

} // namespace

Buffer Server::sendConnectAckMessage(const SConnectMessage& connectMessage, ReasonCode reasonCode, const SClientSession& existingClientSession, const SClientSession& clientSession) const
{
    const auto& protocol = clientSession->protocol();
    const auto& messageWriter = protocol.messageWriter();

    SMessageProperties ackProperties;
    if (reasonCode == ReasonCode::Success && protocol.version() == ProtocolVersion::MqttV5)
    {
        // Per-connection copy: CONNACK properties must never be shared between clients.
        ackProperties = connectAckProperties(*getSettings());
        handleMqtt5ConnectProperties(clientSession, connectMessage, reasonCode);

        const auto connectProperties = connectMessage->getProperties();
        Latency::SNAP_LATENCY(connectProperties, LatencyPhase::ServerWireOut);
        Latency::COPY_LATENCY(connectProperties, ackProperties);
    }

    Buffer messageBuffer(256);
    messageWriter->appendConnectAckToBuffer(messageBuffer, ackProperties, reasonCode, existingClientSession != nullptr);
    // The client may already be gone (e.g. concurrent disconnect cleared the socket) by the time
    // the CONNACK is ready to go out.
    if (const auto& socket = clientSession->getSocket())
    {
        socket->write(messageBuffer);
    }
    return messageBuffer;
}

ReasonCode Server::handleConnectMessage(const SClientSession& newClientSession, const SConnectMessage& connectMessage)
{
    logMessage(LogSubject::Connect, LogPriority::Debug,
               [&connectMessage]
               {
                   return "Received: " + connectMessage->toString() + ".";
               });

    const auto socket = newClientSession->getSocket();
    socket->setOption(IPPROTO_TCP, TCP_NODELAY, 1);
    socket->blockingMode(false);

    if (newClientSession->connectMessageReceived())
    {
        // If the Connect message was already received, terminate the connection
        closeSession(newClientSession);
        return ReasonCode::ProtocolError;
    }

    newClientSession->setConnectMessageReceived(true);

    auto reasonCode = connectMessage->getReasonCode();

    const auto& connectMessageParameters = connectMessage->getParameters();
    const auto  protocolVersion = connectMessageParameters->m_protocolVersion;
    if (newClientSession->protocol().version() != protocolVersion)
    {
        const auto& newProtocol = getGenericProtocols()->getProtocol(protocolVersion);
        newClientSession->setProtocol(newProtocol);
    }

    const auto& clientId = connectMessageParameters->getClientId();

    if (reasonCode == ReasonCode::Success && (m_extensions.authenticating() || m_extensions.authorizing()))
    {
        // The extensions are asked on a thread of their own and the answer arrives later, so this
        // receive worker is released here rather than held for the length of somebody else's
        // directory lookup. Until the answer comes, nothing more is read from this client.
        newClientSession->setAuthenticationPending(true);

        const auto socketAddress = newClientSession->getConnection()
                                       ? std::string(newClientSession->getConnection()->address())
                                       : std::string {};

        // Asked of the socket, because that is what knows. A rule of the form "this client may
        // connect, but only over TLS" is one of the first an authentication extension is asked
        // for, and it can only be written if this is true when it should be.
        const auto encrypted = dynamic_pointer_cast<SSLSocket>(newClientSession->getSocket()) != nullptr;

        m_extensions.authenticate(
            ExtensionHost::AuthRequest {.m_clientId = clientId,
                                        .m_username = connectMessage->getUsername(),
                                        .m_password = connectMessage->getPassword(),
                                        .m_remoteAddress = socketAddress,
                                        .m_protocolVersion = static_cast<uint8_t>(protocolVersion),
                                        .m_encrypted = encrypted},
            [this, newClientSession, connectMessage](const ExtensionHost::AuthDecision decision,
                                                     std::shared_ptr<AclGroup>         aclGroup)
            {
                // Set before the session is let go, so the first PUBLISH it sends is already
                // checked against the right group; after this the pointer is only ever read.
                newClientSession->setAclGroup(std::move(aclGroup));
                newClientSession->setAuthenticationPending(false);

                SClientSession surviving;
                const auto     outcome = completeConnectMessage(newClientSession, connectMessage, decision, &surviving);
                if (outcome == ReasonCode::Success)
                {
                    // The survivor, not the session that carried the CONNECT: a takeover keeps the
                    // existing one, and the bytes to be read again belong to whichever session now
                    // owns the socket.
                    // The survivor for the timer, because that is the session that lives on.
                    (surviving ? surviving : newClientSession)->scheduleIdleDisconnect();
                    // Whatever the client sent after its CONNECT is still in the read buffer and
                    // the socket, and the reactor has no further event to give for it: an
                    // edge-triggered pool reported those bytes once, while this session was
                    // refusing to read them.
                    // Whatever the client sent behind its CONNECT goes to the session that came
                    // out of this, which is the existing one when the CONNECT took a session over.
                    // The session that carried the CONNECT no longer has the socket then, and is
                    // never read again - which is where those bytes used to be lost.
                    const auto& reader = surviving ? surviving : newClientSession;
                    if (auto awaiting = newClientSession->takeBytesAwaitingAuthentication())
                    {
                        reader->adoptPendingBytes(std::move(awaiting));
                    }
                    if (const auto receiveThread = reader->clientSessionReceiveThread())
                    {
                        receiveThread->queueProcessSession(reader);
                    }
                }
                else
                {
                    logMessage(LogSubject::Connect, LogPriority::Error,
                               format("Session rejected, reason: {}.", xmq::toString(outcome)));
                }
            });

        return ReasonCode::Success;
    }

    return completeConnectMessage(newClientSession, connectMessage, ExtensionHost::AuthDecision::NotHandled);
}

ReasonCode Server::completeConnectMessage(const SClientSession& newClientSession, const SConnectMessage& connectMessage,
                                          const ExtensionHost::AuthDecision extensionDecision,
                                          SClientSession*                   survivingSession)
{
    const auto& connectMessageParameters = connectMessage->getParameters();
    const auto  protocolVersion = connectMessageParameters->m_protocolVersion;
    const auto& clientId = connectMessageParameters->getClientId();

    auto reasonCode = connectMessage->getReasonCode();
    auto existingClientSession = getClientSession(clientId);

    if (reasonCode == ReasonCode::Success)
    {
        // Asked of the socket, because a refusal is worth nothing to whoever reads it without
        // saying where it came from - and by the time the answer is logged the connection may be
        // gone.
        const auto remoteAddress = newClientSession->getConnection()
                                       ? string(newClientSession->getConnection()->address())
                                       : string {};

        reasonCode = authenticate(clientId,
                                  connectMessage->getUsername(),
                                  connectMessage->getPassword(),
                                  remoteAddress,
                                  connectMessageParameters->m_protocolVersion,
                                  existingClientSession,
                                  extensionDecision);
    }

    auto existingSessionIsConnected = false;
    auto existingSessionIsClean = true;
    auto clientSession = newClientSession;
    if (reasonCode == ReasonCode::Success)
    {
        if (const auto& messageProperties = connectMessage->getProperties())
        {
            const auto bridgeOrigin = messageProperties->getUserProperty("origin-node");
            clientSession->setBridgeOrigin(string(bridgeOrigin));
        }
        clientSession->setClusterSession(connectMessage->getUsername() == "cluster");

        if (!existingClientSession)
        {
            auto loadedPersistentData = false;
            if (!initializeNewClientSession(newClientSession, connectMessage, loadedPersistentData))
            {
                // Duplicate clientId or other problems
                reasonCode = ReasonCode::ErrorIdentifierRejected;
            }
            if (loadedPersistentData)
            {
                existingClientSession = newClientSession;
            }
        }
        else
        {
            existingSessionIsConnected = existingClientSession->isConnected();
            existingSessionIsClean = existingClientSession->isCleanSession();
            // The takeover moves the connection to the existing session; everything below
            // (CONNACK, session continuation) must operate on that session, not newClientSession.
            clientSession = takeOverExistingClientSession(newClientSession, existingClientSession);
            // One-shot disarms a socket when its event fires, so the socket carrying this CONNECT
            // has to be re-armed once it belongs to the session that took over. Edge-triggered
            // leaves it armed and must be left alone: SocketPool only issues EPOLL_CTL_MOD for a
            // one-shot re-arm, so this would reach EPOLL_CTL_ADD on a socket already in the
            // reactor, fail with EEXIST, and FastTCPServer answers a failed add by closing the
            // connection - the client is left with a socket the broker never reads again, and its
            // DISCONNECT is never seen.
            if (getTriggerMode() == SocketPoolTriggerMode::OneShot)
            {
                watchSession(clientSession, true);
            }
        }
    }

    if (clientSession->protocol().version() != protocolVersion)
    {
        const auto& newProtocol = getGenericProtocols()->getProtocol(protocolVersion);
        clientSession->setProtocol(newProtocol);
    }

    if (const auto& connectMessageProperties = connectMessage->getProperties())
    {
        Latency::SNAP_LATENCY(connectMessageProperties, LatencyPhase::ServerHandler);
        Latency::SNAP_LATENCY(connectMessageProperties, LatencyPhase::ServerDelivery);
    }

    const Buffer messageBuffer = sendConnectAckMessage(connectMessage, reasonCode, existingClientSession, clientSession);

    if (reasonCode != ReasonCode::Success)
    {
        closeSession(newClientSession);
        return reasonCode;
    }

    if (systemStatistics())
    {
        systemStatistics()->registerConnectedClient(existingSessionIsConnected, existingSessionIsClean);
        systemStatistics()->registerSentData(messageBuffer.bytes(), 1, 0);
    }

    if (existingClientSession)
    {
        if (connectMessageParameters->m_cleanSession != 0)
        {
            clientSession->clearSession(connectMessage);
        }
        else
        {
            clientSession->continueSession(connectMessage);
        }
    }

    logMessage(LogSubject::Connect, LogPriority::Debug,
               [&clientId, clientSession]
               {
                   return format("{} uses {} protocol.", clientId, to_string(clientSession->getProtocolVersion()));
               });

    // The address goes as an attribute rather than a field: a fact the broker knows about an event
    // should not cost an ABI version to publish. Copied only if some extension asked for it.
    const auto remoteAddress = clientSession->getConnection()
                                   ? std::string(clientSession->getConnection()->address())
                                   : std::string {};

    m_extensions.publishEvent(XMQ_EVENT_CLIENT_CONNECTED, clientId, clientSession->getUsername(), {}, 0, 0,
                              false, {{ExtensionHost::remoteAddressAttribute, remoteAddress}});

    // Which session came out of this, for a caller that has more to do with it. A takeover keeps
    // the existing session and discards newClientSession, and the comment above that swap says
    // everything after it must use the survivor - but the asynchronous connect path is outside this
    // function and had no way to know which one that was. It queued newClientSession to be read
    // again, so a SUBSCRIBE pipelined behind a CONNECT that took over a session was never processed.
    if (survivingSession != nullptr)
    {
        *survivingSession = clientSession;
    }

    return ReasonCode::Success;
}

void Server::handleMqtt5ConnectProperties(const SClientSession& clientSession, const SConnectMessage& connectMessage, ReasonCode& reasonCode)
{
    using enum Property;

    const auto properties = connectMessage->getProperties();
    if (!properties)
    {
        return;
    }
    reasonCode = properties->validate();

    // Only what the client is telling us about itself, and only into the session. This used to walk
    // every integer property the client sent and copy each one into the CONNACK on the way past,
    // which meant the answer reported the client's own limits back to it as the server's: a client
    // asking for ReceiveMaximum 10 was told the server accepts 10, when it accepts 32768, and duly
    // throttled itself. The same for MaximumPacketSize and TopicAliasMaximum. Nothing caught it
    // because the load generator sends no CONNECT properties at all, so every measurement took the
    // one path where the fault cannot appear.
    //
    // The CONNACK's properties are now built in one place, from the server's settings, and nothing
    // else writes to them.
    if (int64_t value = 0;
        clientSession && properties->getProperty(MaximumPacketSize, value))
    {
        clientSession->setMaximumPacketSize(static_cast<size_t>(value));
    }

    if (int64_t value = 0;
        clientSession && properties->getProperty(ReceiveMaximum, value))
    {
        clientSession->setMaxInflightMessages(static_cast<uint16_t>(value));
    }

    if (int64_t value = 0;
        clientSession && properties->getProperty(TopicAliasMaximum, value))
    {
        clientSession->setTopicAliasMaximum(static_cast<uint16_t>(value));
    }
}

namespace {

/**
 * @brief How to say "ask again later" to a client that cannot be authenticated right now.
 *
 * Not a failed authentication. The client's credentials were never judged - nothing that could
 * judge them was reachable - and telling it they were wrong sends a person to reset a password
 * that was fine, and a device into the retry-with-new-credentials path it keeps for that answer.
 * Both codes below mean the server, not the client, is the reason.
 */
ReasonCode unavailableFor(const ProtocolVersion protocolVersion)
{
    return protocolVersion == ProtocolVersion::MqttV5 ? ReasonCode::ServerUnavailable
                                                      : ReasonCode::ErrorServerNotAvailable;
}

} // namespace

ExtensionHost::Report Server::reloadExtensions()
{
    // The broker's own configuration may have moved the accounts since the last time; an extension
    // asked to re-read its settings should not be answered with yesterday's address.
    m_extensions.userDatabaseUri(getSettings()->userDatabaseUri());

    // Read afresh from disk, exactly as start-up does, so there is one way a configuration reaches
    // the broker and not two that can disagree.
    return m_extensions.reloadSettings(
        ExtensionHost::readConfiguration(getSettings()->configurationPath(), *getLogEngine()));
}

vector<ExtensionHost::Description> Server::describeExtensions() const
{
    return m_extensions.describe();
}

ExtensionHost::Report Server::switchExtension(const string& name, const bool on)
{
    // Written before anything is switched, for the same reason a changed setting is: a decision
    // that cannot be recorded must not be acted on, or a restart quietly undoes it and nothing says
    // why. It is also required for enabling - the configuration is read again below, and an entry
    // still marked disabled would not be found.
    auto written = m_extensions.writeEnabled(name, on);
    if (written.failed())
    {
        return written; // refusedWrite has already said why, in the log and here
    }

    if (!on)
    {
        written += m_extensions.disable(name);
        return written;
    }

    // Read afresh, because the entry may have been edited while the extension was off - and because
    // there is one way a configuration reaches the broker, not two that can disagree.
    const auto configured = ExtensionHost::readConfiguration(getSettings()->configurationPath(),
                                                             *getLogEngine());
    const auto wanted = ranges::find_if(configured, [&name](const auto& candidate)
                                        {
                                            return candidate.m_name == name;
                                        });
    if (wanted == configured.end())
    {
        return ExtensionHost::Report::problem(name + ": the configuration no longer lists it");
    }
    written += m_extensions.enable(*wanted);
    return written;
}

ExtensionHost::Report Server::setExtensionSettings(const string& name, const map<string, string>& settings)
{
    auto report = m_extensions.writeSettings(name, settings);
    if (report.failed())
    {
        // Nothing was written, so there is nothing to apply and re-reading the file would report
        // the settings the extension already has as though they had just been accepted.
        return report;
    }

    // Applied by re-reading the file that was just written, rather than from what arrived over the
    // wire: what the broker runs is then what is on disk, and the two cannot disagree.
    report += reloadExtensions();
    return report;
}

ReasonCode Server::authenticate(const string&                     clientId,
                                const string&                     username,
                                const string&                     password,
                                const string&                     remoteAddress,
                                const ProtocolVersion             protocolVersion,
                                const shared_ptr<ClientSession>&  existingClientSession,
                                const ExtensionHost::AuthDecision extensionDecision)
{
    if (constexpr auto maxClientIdLength = 255;
        clientId.empty() || clientId.size() > maxClientIdLength)
    {
        logMessage(LogSubject::ServerConnections, LogPriority::Error,
                   [&clientId]
                   {
                       auto error = clientId.empty()
                                        ? "Empty client identifier."
                                        : "Invalid client identifier: '" + clientId + "'.";
                       return error;
                   });
        m_extensions.publishError(LogSubject::Connect, "invalid_client_id",
                                  clientId.empty() ? "Empty client identifier"
                                                   : "Invalid client identifier",
                                  clientId, username,
                                  {{ExtensionHost::remoteAddressAttribute, remoteAddress}});
        return protocolVersion == ProtocolVersion::MqttV5 ? ReasonCode::IdentifierRejected : ReasonCode::ErrorIdentifierRejected;
    }

    switch (extensionDecision)
    {
        case ExtensionHost::AuthDecision::Allow:
            // An extension vouched for this client. Its own accounts are not consulted: the point
            // of an enterprise directory is that it, and not a file on the broker, is the register.
            break;

        case ExtensionHost::AuthDecision::Deny:
            logMessage(LogSubject::ServerConnections, LogPriority::Error,
                       [&clientId]
                       {
                           return clientId + ": refused by an extension.";
                       });
            m_extensions.publishError(LogSubject::Connect, "refused", "Refused by an extension",
                                      clientId, username,
                                      {{ExtensionHost::remoteAddressAttribute, remoteAddress}});
            return ReasonCode::ErrorAuthenticationFailed;

        case ExtensionHost::AuthDecision::Unavailable:
            // Nobody could be asked. Admitting the client would mean admitting everyone for as
            // long as the outage lasts, which is the one failure an authenticator must not have.
            logMessage(LogSubject::ServerConnections, LogPriority::Error,
                       [&clientId]
                       {
                           return clientId + ": refused, no authentication thread was free.";
                       });
            m_extensions.publishError(LogSubject::Connect, "no_authentication_thread",
                                      "No authentication thread was free", clientId, username,
                                      {{ExtensionHost::remoteAddressAttribute, remoteAddress}});
            return unavailableFor(protocolVersion);

        case ExtensionHost::AuthDecision::SubsystemError:
            // The extension answered, and its answer was that it could not reach what it
            // authenticates against. Its own accounts are not tried: a store that cannot be read
            // must not quietly become a store that says yes.
            logMessage(LogSubject::ServerConnections, LogPriority::Error,
                       [&clientId]
                       {
                           return clientId + ": refused, an extension could not reach its authentication store.";
                       });
            m_extensions.publishError(LogSubject::Connect, "unavailable",
                                      "An extension could not reach its authentication store",
                                      clientId, username,
                                      {{ExtensionHost::remoteAddressAttribute, remoteAddress}});
            return unavailableFor(protocolVersion);

        case ExtensionHost::AuthDecision::NotHandled:
            // Nothing claimed this client. A name is refused here, and the broker's own accounts are
            // not consulted: from 0.9.16 an MQTT client is admitted by an extension or not at all,
            // and a fallback would make "authentication is an extension's job" true only until the
            // extension was missing. UserManager stays for the web interface, which has to work in
            // order to repair a broken extension - and for the one decision below.
            //
            // A client with no name is different. Whether anonymous access is allowed is the
            // broker's policy, written in its configuration, and an extension abstains on an empty
            // username precisely so that this is decided in one place.
            if (username.empty())
            {
                if (!getSettings()->userManager().isAllowAnonymous())
                {
                    logMessage(LogSubject::ServerConnections, LogPriority::Error,
                               [&clientId]
                               {
                                   return clientId + ": anonymous connections are not allowed.";
                               });
                    m_extensions.publishError(
                        LogSubject::Connect, "anonymous_refused", "Anonymous connections are not allowed",
                        clientId, username, {{ExtensionHost::remoteAddressAttribute, remoteAddress}});
                    return ReasonCode::ErrorAuthenticationFailed;
                }
                break;
            }

            logMessage(LogSubject::ServerConnections, LogPriority::Error,
                       [&clientId, this]
                       {
                           return clientId + (m_extensions.authenticating()
                                                  ? ": no extension recognised this account."
                                                  : ": refused, because no extension authenticates. "
                                                    "Configure an authenticator in xmq_extensions.d.");
                       });

            // The reason is finer than what the client is told, which learns only that it failed:
            // an extension counting attempts has to tell "nobody knows this name" from "there is
            // nothing here that could know it".
            m_extensions.publishError(
                LogSubject::Connect,
                m_extensions.authenticating() ? "no_account" : "no_authenticator",
                "Invalid username or password", clientId, username,
                {{ExtensionHost::remoteAddressAttribute, remoteAddress}});

            return ReasonCode::ErrorAuthenticationFailed;
    }

    const auto logDebugMessages = loggerHas(LogPriority::Debug);
    if (logDebugMessages)
    {
        logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                   [&clientId]
                   {
                       return clientId + " authenticated.";
                   });
    }

    if (existingClientSession && existingClientSession->isConnected())
    {
        closeSession(existingClientSession, true);
        if (logDebugMessages)
        {
            logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                       [&clientId]
                       {
                           return clientId + " is taking over the existing session.";
                       });
        }
    }

    return ReasonCode::Success;
}

std::string Server::getClusterPassword() const
{
    // Absent on a server that has never been clustered, and asked for regardless: this node's own
    // cluster entry is built at startup, whether or not there is a cluster. Reported as no
    // password rather than by throwing, which took the whole broker down over an account only
    // cluster peers ever use.
    // The configuration first, which is where this belongs: it is a secret to be presented, and a
    // presented secret has to be readable. The accounts database holds only a verifier for it, so
    // that incoming peers can be checked without anyone being able to read the secret back out.
    if (const auto configured = getSettings()->clusterPassword();
        !configured.empty())
    {
        return configured;
    }

    // Nothing else can answer: the account holds a verifier, which is not something to present.
    // The configuration is filled from the accounts on the last reading that could still see the
    // secret, so an empty answer here means a node that has never had one.
    return {};
}

void Server::attachToCluster(const Host& host, const bool encrypted) const
{
    m_cluster.load()->joinCluster(host, encrypted);
}

void Server::detachFromCluster() const
{
    m_cluster.load()->detachCluster();
}

namespace {

/// The refusal code for the protocol the client is speaking. MQTT 5 has a code that says why;
/// earlier versions do not, and get the general one.
ReasonCode refusalFor(const SClientSession& client)
{
    return client->getProtocolVersion() == ProtocolVersion::MqttV5 ? ReasonCode::NotAuthorized
                                                                   : ReasonCode::UnspecifiedError;
}

/**
 * @brief What an authorizing extension says about this session and this topic.
 *
 * Reached only when such an extension is loaded, and then it is a hash lookup on a table shared by
 * every session in the group; the extension itself is called once per group and topic and not
 * again. A broker with no such extension does not get this far - the caller tests aclGroup()
 * first, which is a null check on a pointer the session already holds.
 *
 * A cluster session is not asked about. Traffic between nodes is the broker's own, and running it
 * past a customer's topic rules would let a rule mistake partition the cluster.
 */
ReasonCode extensionGrants(const SClientSession& client, const Topic* topic, const xmq_acl_action action)
{
    if (client->isClusterSession())
    {
        return ReasonCode::Success;
    }
    switch (client->aclGroup()->authorize(topic->fullName(), action))
    {
        case AclDecision::Deny:
        case AclDecision::SubsystemError:
            // Refused the same way, and only this one operation: the session stays, so the client
            // may try again once the store is back. Told apart in the log by the host, not here -
            // this runs at message rate.
            return refusalFor(client);
        case AclDecision::Allow:
        case AclDecision::NotHandled:
            break;
    }
    return ReasonCode::Success;
}

} // namespace

ReasonCode Server::grantPublish(const SClientSession& client, const Topic* topic)
{
    using enum ReasonCode;

    if (client->aclGroup() && extensionGrants(client, topic, XMQ_ACL_PUBLISH) != Success)
    {
        return refusalFor(client);
    }

    if (!topic->isSystem())
    {
        if (client->isClusterSession())
        {
            return Success;
        }
        if (!topic->isCluster())
        {
            return Success;
        }
    }
    return refusalFor(client);
}

ReasonCode Server::grantSubscription(const SClientSession& client, const Topic* topic)
{
    using enum ReasonCode;

    if (client->aclGroup() && extensionGrants(client, topic, XMQ_ACL_SUBSCRIBE) != Success)
    {
        return refusalFor(client);
    }

    if (client->isClusterSession())
    {
        return Success;
    }
    if (!topic->isCluster())
    {
        return Success;
    }
    return refusalFor(client);
}

void Server::watchSession(const SClientSession& clientSession, const bool rearmOneShot)
{
    try
    {
        if (const auto& socket = clientSession->getSocket();
            socket && socket->active()) // Is the server running?
        {
            watchConnection(clientSession->getConnection(), rearmOneShot);
        }
    }
    catch (const Exception& e)
    {
        CERR(e.what());
    }
}

void Server::unwatchSession(const SClientSession& clientSession)
{
    if (clientSession->isConnected())
    {
        unwatchConnection(clientSession->getConnection());
        //COUT("Unwatching connection " << clientSession->getClientId() << ", socket " << clientSession->getConnection()->getSocket()->fd());
    }
}

shared_ptr<Subscription> Server::subscribeClient(const SClientSession& client, const Destination& destination, const uint32_t subscriptionId) const
{
    return m_subscriptionManager->subscribe(destination.m_topic, client,
                                            static_cast<Qos>(destination.m_subscribeOptions.m_maxQos), subscriptionId,
                                            destination.m_subscribeOptions);
}

void Server::unsubscribeClient(ClientSession* clientSession, const Destination& destination) const
{
    m_subscriptionManager->unsubscribe(destination.m_topic, clientSession);
}

void Server::clear()
{
    m_clientSessionThreads.stop();
    m_subscriptionManager->clear();
    getClientSessionManager()->clear();
}

[[maybe_unused]] std::vector<CConnectionInfo> Server::getClientConnectionsInfo(const RegularExpression& matchClientId) const
{
    return getClientSessionManager()->getClientConnectionsInfo(matchClientId);
}

ClientSessionThreads Server::getClientSessionThreads()
{
    return m_clientSessionThreads.getClientSessionThreads();
}

void Server::publishMessage(const SPublishMessage& publishMessage) const
{
    m_messageDeliveryThreads.publishMessage(publishMessage);
}

const std::set<Server*>& Server::instances()
{
    std::scoped_lock lock(m_instancesMutex);
    return m_instancesSet;
}
