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
#include <mutex>

#include "base/MessageDispatch.h"
#include "base/MessageQueue.h"
#include "base/ReasonCode.h"
#include "common/BaseClientSession.h"
#include "common/GenericProtocol.h"
#include "common/GenericProtocols.h"
#include "common/PacketReader.h"
#include "common/SessionThreads.h"
#include <sptk5/net/SocketEvents.h>
#include <tuple>

namespace xmq {

class SessionThread;

namespace client {

constexpr auto defaultConnectTimeout = 60;

struct ConnectParameters
{
    std::chrono::seconds               m_keepAliveInterval {60};
    std::chrono::seconds               m_connectTimeout {defaultConnectTimeout};
    SLastWillInfo                      m_lastWillInfo;
    std::function<void(sptk::Buffer&)> m_tweakMessage {};
    uint16_t                           m_maxInflightMessages {0};
    bool                               m_cleanSession {true};
};

/**
 * @brief Test client session.
 */
class Session final
    : public BaseClientSession
    , public std::enable_shared_from_this<Session>
{
    friend class MqttClient;

public:
    enum class State : uint8_t
    {
        Disconnected = 0,
        Connecting = 1,
        Connected = 2,
        Disconnecting = 3
    };

    /**
     * @brief Size the shared socket-event pool for about this many sessions.
     *
     * The pool is one static object shared by every session in the process, and its registration
     * map holds one entry per connected socket. Growing that map rehashes everything it holds
     * under the lock every event needs: a client opening 2500 connections a second stops on it
     * 140 seconds in, where std::unordered_map passes 351062 entries, and again at 285. An
     * application that knows how many sessions it is about to open says so here and never meets
     * the event. Called before connecting; harmless afterwards.
     * @param sessionCount Sessions to size for.
     */
    static void reserveSessionPool(std::size_t sessionCount)
    {
        m_socketEvents.reserve(sessionCount);
    }

    /**
     * @brief Constructor.
     * @remark Session must only be created via std::shared_ptr.
     * @param messageCallback   Message callback executed on message arrival (not on Ack messages).
     * @param logger            Logger.
     * @param bindAddress       Optional bind address as a local interface IP address.
     */
    explicit Session(const MessageCallback& messageCallback, const std::shared_ptr<sptk::Logger>& logger, const std::string& bindAddress = "");

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /**
     * @brief Destructor.
     */
    ~Session() override;

    /**
     * @brief Connect the client to a server.
     * @param host              Server host.
     * @param credentials       Client credentials.
     * @param protocolVersion   MQTT protocol version.
     * @param connectParameters Connect parameters.
     * @param connectMessageProperties Connect message properties.
     * @param sslKeys           SSL keys, if SSL connection is required.
     * @param onConnectCompleted
     * @return connection result (ReasonCode).
     */
    ReasonCode connect(const sptk::Host& host, const ConnectCredentials& credentials, ProtocolVersion protocolVersion,
                       const ConnectParameters& connectParameters, const SMessageProperties& connectMessageProperties,
                       const std::shared_ptr<sptk::SSLKeys>& sslKeys, const MessageCallback& onConnectCompleted);

    /**
     * @brief Returns true if the clean session flag is set.
     */
    bool isCleanSession() const override;

    /**
     * @brief Register received ack, and send default reply.
     */
    void receivedAck(const MessageId&, Message::Type) override
    {
    }

    /**
     * @brief Queue a message to the inflight queue, which forwards it to the socket.
     * @remark Must be called WITHOUT holding m_mutex: the queue's forward callback acquires it.
     * @param messageDispatch   Message delivery.
     */
    void sendMessage(SMessageDispatch& messageDispatch) override;

    /**
     * @brief Post a message to the client send queue.
     * @param message           Message to send.
     * @param retain            The retain message flag.
     * @returns Message ID.
     */
    MessageId postMessage(const SMessage& message, const bool retain)
    {
        return postMessage(message, message->getQos(), {}, retain);
    }

    /**
     * @return True, if the hangup signal is received from the connection.
     */
    bool hasHangup() const;

    /**
     * @brief Immediately close connection.
     * @param serverHangupReason    Reason reported to the disconnect callback.
     * @param abortive              If true, close with RST (abrupt). If false, close in an
     *                              orderly way (FIN), so sent DISCONNECT is delivered first.
     */
    void hangup(ReasonCode serverHangupReason, bool abortive = true);
    void onDisconnect(MessageCallback messageCallback);

    bool isConnected() const override;

    void enableKeepAlive(bool enable);
    bool waitForDisconnectSent(const std::chrono::milliseconds& timeout);
    void queueReceiveMessages();

    [[nodiscard]] const std::string& getClientId() const override;
    [[nodiscard]] std::string_view   getClientIdUnlocked() const override;
    [[nodiscard]] std::string_view   getUsername() const override;
    [[nodiscard]] std::string        prefix() const override;
    void                             subscribedTo(const std::shared_ptr<Subscription>&, Qos, SubscriptionOptions) override
    {
    }

    // Methods that are not needed for non-persistent sessions:
    [[nodiscard]] int64_t  recordId() const override;
    [[nodiscard]] SStorage storage() const override;

    [[nodiscard]] static auto getTopicManager()
    {
        return m_topicManager;
    }

    [[nodiscard]] static const Topic* getTopic(const std::string_view topic)
    {
        return m_topicManager->getTopic(topic);
    }

    [[nodiscard]] ProtocolVersion getProtocolVersion() const
    {
        return m_protocolVersion;
    }

    /**
     * @brief Post a message to the client send queue.
     * @param message           Message to send.
     * @param qos               Message QOS.
     * @param subscriptionIds   Subscription IDs (ignored).
     * @param retain            The retain message flag.
     */
    MessageId postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet& subscriptionIds, bool retain) override;

    SendReceiveResult receiveMessages() override;

protected:
    /**
     * @brief Keep to the Receive Maximum the server announced in its CONNACK.
     *
     * MQTT 5 section 4.9: a server says how many QoS 1 and 2 publications it accepts
     * unacknowledged, and sending more is a protocol error it disconnects for. HiveMQ announces 10
     * and enforces it - every publisher of a load test was dropped within seconds.
     */
    void applyServerReceiveMaximum(const SMessage& connectAck);

    void autoAck(const Message* message) override;

private:
    std::string                        m_clientId;                                           ///< Client id.
    std::string                        m_bindAddress;                                        ///< Bind address.
    std::string                        m_username;                                           ///< Username.
    std::string                        m_prefix;                                             ///< Log message prefix.
    ConnectParameters                  m_connectProperties;                                  ///< Connect parameters.
    std::atomic<State>                 m_state {State::Disconnected};                        ///< Session state.
    bool                               m_isHangup {false};                                   ///< Flag: has the server hang up.
    bool                               m_isMonitoring {false};                               ///< Flag: are the incoming events being monitored.
    std::shared_ptr<SessionThread>     m_sessionThread {m_allClientThreads.getNextThread()}; ///< Session thread.
    /**
     * The pending keep-alive event, and the small lock that is all it needs.
     *
     * Its own lock and not m_mutex: the timer callback re-arms through here, and m_mutex is held
     * by the receive path for as long as it takes to parse a batch. Coupling the two put the one
     * timer thread of the whole process behind the busiest session in it.
     */
    mutable std::mutex                 m_keepAliveMutex;
    sptk::STimerEvent                  m_keepAliveEvent;                                     ///< Optional keep alive timer event.
    /**
     * When the session must ping if it sends nothing else before then.
     *
     * Atomic, and deliberately not guarded by m_mutex. One static timer serves every session in
     * the process, on one thread that runs its callbacks in turn; a callback that took m_mutex
     * queued behind the receive path, which holds that lock while it parses. At 100000 sessions
     * the timer fell a hundred seconds behind, a publisher whose inflight window had filled lost
     * the ping that was its only traffic, and the broker closed it for a keep-alive timeout - the
     * "Not connected" that ended every 100k/s run.
     */
    std::atomic<sptk::DateTime::time_point> m_nextKeepAliveTime;

    /// The interval, kept beside the time so the timer callback needs nothing under a lock.
    std::atomic<std::chrono::seconds>  m_keepAlivePeriod {std::chrono::seconds {0}};
    sptk::Semaphore                    m_isDisconnectSent;                                   ///< Is disconnect sent flag.
    sptk::Buffer                       m_readBuffer;                                         ///< Inbound read buffer (drained in batches).
    size_t                             m_readConsumed {0};                                   ///< Bytes already parsed from m_readBuffer.
    sptk::Buffer                       m_ackBatchBuffer;                                     ///< Acks accumulated during a drain, flushed with a single write. Guarded by m_mutex.
    std::shared_ptr<sptk::Logger>      m_logger;                                             ///< Logger.
    MessageCallback                    m_onConnectCompleted;                                 ///< Optional callback called for completed connection.
    MessageCallback                    m_onDisconnect;                                       ///< Optional callback called for disconnection.
    ProtocolVersion                    m_protocolVersion {ProtocolVersion::MqttV5};          ///< MQTT protocol version.
    static SessionThreads              m_allClientThreads;                                   ///< Session threads.
    static sptk::Timer                 keepAliveTimer;                                       ///< Keep alive timer.
    static sptk::SocketEvents<Session> m_socketEvents;                                       ///< Socket event manager.
    static STopicManager               m_topicManager;                                       ///< Shared topic manager.
    static GenericProtocols            m_genericProtocols;                                   ///< Shared generic protocols.

    /**
     * @brief Send the 'Connect' message to the server.
     * @param host              Server host.
     * @param credentials       Server connection credentials.
     * @param protocolVersion   MQTT protocol version.
     * @param connectParameters Connection parameters.
     * @param connectMessageProperties Connect message properties.
     * @param sslKeys           SSL keys.
     */
    void       sendConnect(const sptk::Host& host, const ConnectCredentials& credentials, ProtocolVersion protocolVersion, const ConnectParameters& connectParameters, const SMessageProperties& connectMessageProperties, const std::shared_ptr<sptk::SSLKeys>& sslKeys);
    ReasonCode receiveConnect();

    /**
     * @brief Set the session state without validation.
     * @remark Caller MUST hold m_mutex.
     */
    void setStateUnlocked(const State state)
    {
        m_state = state;
    }

    /**
     * @brief Transition session state if the current state matches @p from.
     * @remark Caller MUST hold m_mutex.
     * @return true if transition succeeded, false otherwise.
     */
    bool transitionUnlocked(const State from, const State to)
    {
        if (m_state != from)
        {
            return false;
        }
        m_state = to;
        return true;
    }

    void monitorIncomingData(bool monitor);
    void monitorIncomingDataUnlocked(bool monitor);
    void fillReadBufferUnlocked(sptk::TCPSocket* socket);
    void discardConsumedReadBufferUnlocked();

    /**
     * @brief Serialize the ack for a received message into m_ackBatchBuffer.
     * @remark Caller MUST hold m_mutex.
     * @param message           Received message requiring an ack.
     */
    void appendAckBatchUnlocked(const Message& message);

    /**
     * @brief Write the accumulated ack batch to the socket in a single call.
     * @remark Caller MUST hold m_mutex.
     */
    void flushAckBatchUnlocked();

    /**
     * @brief Directly send a message to the client.
     * @remark Caller MUST hold m_mutex. Reached from two places that both hold it: the
     *         inflight queue's forward callback (which also holds the queue mutex — never
     *         acquire the queue mutex while holding m_mutex, or it deadlocks) and autoAck().
     * @param messageDispatch            Message delivery.
     */
    /// The queue hands a message back when its turn comes; this session takes its lock and writes it.
    void forwardMessage(const SMessageDispatch& messageDispatch) override
    {
        const std::unique_lock lock(m_mutex);
        sendMessageUnlocked(*messageDispatch);
    }

    void sendMessageUnlocked(const MessageDispatch& messageDispatch);

    void closeSocketUnlocked(bool abortive = true);
    void stopKeepAlive();

    void                                         scheduleKeepAlive(bool init = false);
    std::tuple<sptk::DateTime::time_point, bool> nextKeepAlive();

    bool isSendQueueEmpty() const;

    ReasonCode doConnect(const sptk::Host& host, const ConnectCredentials& credentials,
                         ProtocolVersion protocolVersion, const ConnectParameters& connectParameters,
                         const SMessageProperties&             connectMessageProperties,
                         const std::shared_ptr<sptk::SSLKeys>& sslKeys, const MessageCallback& onConnectCompleted);
};

using SSession = std::shared_ptr<Session>;

} // namespace client
} // namespace xmq
