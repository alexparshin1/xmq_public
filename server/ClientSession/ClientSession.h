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

#include "common/AtomicSharedPtr.h"
#include "../Subscription/Subscription.h"
#include "ClientConnection.h"
#include "ClientSessionThread.h"
#include "SocketWriteBuffer.h"
#include "base/Message.h"
#include "base/MessageProperties.h"
#include "base/ProtocolException.h"
#include "common/ConnectMessage.h"
#include "common/MessageRouter.h"
#include "common/mqtt/PacketReaderMqtt.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace xmq {

class Server;
class SystemStatistics;

/**
 * @brief Client session on the server.
 */
class XMQ_EXPORT ClientSession final
    : public ClientConnection
    , public MessageRouter
{
    friend class ClientSessionThread;
    friend class ClientSessionReceiveThread;
    friend class Subscriptions;
    friend class SubscriptionManager;

public:
    /**
     * @brief Discard anything left in the inbound read buffer.
     *
     * A session that is not clean outlives the connection it was reading from, and whatever
     * partial frame that connection left behind is meaningless to the next one. Carried over, it
     * is prepended to the first bytes read from the new connection, so every packet after that is
     * parsed at the wrong offset - and a misframed packet looks incomplete, so it is neither
     * decoded nor reported, and the session goes quiet while its connection stays open.
     */
    void discardReadBuffer()
    {
        // Requested here, performed by the receive thread. The buffer belongs to that thread -
        // receiveMessages() reads into it and consumes from it without a lock - so clearing it
        // from the thread performing a takeover is two threads mutating one sptk::Buffer: it
        // corrupts the heap, and it can discard bytes already read but not yet parsed. A client
        // whose SUBSCRIBE is sitting in that buffer loses it, which is the failure this whole
        // path exists to prevent.
        m_discardReadBuffer.store(true, std::memory_order_release);
    }

    /**
     * @brief Constructor.
     * @param server                    Server.
     * @param connectMessageParameters  Session parameters.
     * @param connectMessageProperties  Connect message properties.
     */
    explicit ClientSession(Server*                          server,
                           const SConnectMessageParameters& connectMessageParameters = std::make_shared<ConnectMessageParameters>(),
                           const SMessageProperties&        connectMessageProperties = {});

    /**
     * @brief Factory.
     * Creates a client connection object and makes it persistent
     * @param server                    Server.
     * @param connectMessageParameters  Session parameters.
     * @param connectMessageProperties  Connect message properties.
     */
    static std::shared_ptr<ClientSession> factory(Server*                          server,
                                                  const SConnectMessageParameters& connectMessageParameters = std::make_shared<ConnectMessageParameters>(),
                                                  const SMessageProperties&        connectMessageProperties = {});

    /**
     * @brief Destructor.
     */
    ~ClientSession() noexcept override;

    /**
     * @brief Load prior session data.
     * @param clientId          Load session data.
     */
    static void loadSession(const std::string& clientId);

    /**
     * @brief Clear session.
     * Removes any queued messages and existing subscriptions.
     * Initializes connection parameters.
     * @param connectMessage    The Connect message.
     */
    void clearSession(const SConnectMessage& connectMessage);

    /**
     * @brief Clear session
     * Removes any queued messages and existing subscriptions.
     */
    void clearSession();

    /**
     * @brief Continue the session.
     * Preserves queued messages and existing subscriptions.
     * @param connectMessage    The Connect message.
     */
    void continueSession(const SConnectMessage& connectMessage);

    /**
     * @brief Disconnect session by sending a Disconnect message.
     * @param reasonCode        Disconnect reason's code.
     * @param reasonString      Disconnect reason's description (for logging).
     */
    void disconnect(ReasonCode reasonCode, const std::string& reasonString = "");

    /**
     * @brief Close session.
     * Closes network connection.
     */
    void closeSession() override;

    /**
     * @brief Close the session, but only while it is still holding a given connection.
     *
     * Closing a session is several steps and holds no lock across all of them, so a reconnect can
     * hand the session a different connection part-way through. The check is made under the
     * session's own lock, immediately before anything is torn down, so a close that has been
     * overtaken by a takeover does nothing rather than dropping the replacement's socket.
     *
     * @param expectedConnection Connection the caller meant to close, or null to close regardless.
     * @return False when this close was overtaken and did nothing, so that a caller with further
     *         teardown of its own can skip it too.
     */
    bool closeSession(const std::shared_ptr<ServerConnectionExt>& expectedConnection);

    /**
     * @brief Count a successful MQTT CONNECT once, paired with the close of this connection.
     * @param statistics               Server statistics to update.
     * @param existingSessionIsClean   Whether the existing session is clean.
     */
    void registerConnectedClient(SystemStatistics& statistics, bool existingSessionIsClean);

    /**
     * @brief Get the time of the most recent successful CONNECT.
     * @return Unix seconds, or zero for a restored offline session.
     */
    [[nodiscard]] std::time_t connectedAt() const
    {
        return m_connectedAt.load(std::memory_order_relaxed);
    }

    /**
     * @brief Whether the close that last took this session's socket away named a connection.
     *
     * A breadcrumb for the reconnect tests, and deliberately the cheapest one possible: a single
     * relaxed store on the path that clears the connection, no lock, no formatting, nothing that
     * could widen the window it is meant to describe. False means the socket was removed by a
     * close that named no connection, and so skipped the comparison that protects a session which
     * has just been handed a new one.
     */
    [[nodiscard]] bool lastCloseNamedItsConnection() const
    {
        return m_lastCloseNamedItsConnection.load(std::memory_order_relaxed);
    }

    /**
     * @brief Records which call site is about to close this session. See lastCloseSite().
     */
    void noteCloseSite(const int site)
    {
        m_lastCloseSite.store(site, std::memory_order_relaxed);
    }

    /**
     * @brief The call site of the close that last removed this session's socket.
     *
     * 1 read failure, 2 keep-alive timeout, 3 end of DISCONNECT, 4 no CONNECT in time,
     * 5 batched write failure. 0 means nothing has recorded one.
     */
    [[nodiscard]] int lastCloseSite() const
    {
        return m_lastCloseSite.load(std::memory_order_relaxed);
    }

    /**
     * @brief Schedule keepalive disconnect.
     * @details If keep alive is enabled and there are no incoming messages.
     * For 1.5 times the keep alive timeout, disconnect.
     */
    void scheduleIdleDisconnect();

    /**
     * @brief What a session is waiting for.
     *
     * The state names the deadline: every session waits for one thing at a time, and which thing it
     * is follows from where the session is in its life rather than from a search through everything
     * it might be waiting for. What a queued message's expiry is waiting for is not one of these -
     * that clock belongs to the queue and runs whatever the session is doing - so it is armed
     * beside the state's own deadline rather than inside it.
     */
    enum class SessionState : uint8_t
    {
        Accepted,       ///< A connection, no CONNECT yet: waiting for the CONNECT packet.
        Authenticating, ///< The CONNECT is being decided on: still waiting, on the same deadline.
        Active,         ///< Connected and accepted: waiting for the client to fall silent.
        Detached,       ///< No connection, the session kept: waiting for it or its queue to expire.
        Gone            ///< Finished with: waiting for nothing.
    };

    /**
     * @return What this session is waiting for.
     */
    [[nodiscard]] SessionState sessionState() const
    {
        return m_sessionState.load(std::memory_order_relaxed);
    }

    /**
     * @brief Act on whatever deadline has passed, then arm the timer again.
     *
     * Runs on the timer thread when the session's event fires.
     */
    void onSessionTimerEvent();

    /**
     * @brief Move the session to another state and arm the timer for what that state waits for.
     *
     * The caller must hold the session lock.
     */
    void setSessionStateUnlocked(SessionState newState);

    /**
     * @brief Arm the session's timer event at the earliest deadline it is waiting on.
     *
     * A deadline that moves further away costs nothing here: whatever is armed is left to fire,
     * find nothing due, and arm itself at the next one. Only a deadline that moves closer than
     * what is armed schedules again. The caller must hold the session lock.
     */
    void armSessionTimerUnlocked();

    /**
     * @return The deadline this session's state waits for, or a default time point when that state
     *         waits for nothing. The caller must hold the session lock.
     */
    [[nodiscard]] sptk::DateTime::time_point deadlineForStateUnlocked() const;

    /**
     * @return The deadline to arm the timer at: what the state waits for, and the queue's own
     *         expiry, which is not a state and runs beside it. The caller must hold the session lock.
     */
    [[nodiscard]] sptk::DateTime::time_point armingDeadlineUnlocked() const;

    /**
     * @return When a silent client stops being tolerated - the last message from it plus one and a
     *         half keep-alive intervals - or a default time point when keep-alive is switched off.
     *         The caller must hold the session lock.
     */
    [[nodiscard]] sptk::DateTime::time_point keepAliveDeadlineUnlocked() const;

    /**
     * @brief Schedule session expiration.
     * @details If the session is not active and not in clean session mode, and session expiration is set,
     *          then the session is scheduled for removal.
     */
    void scheduleExpiration();

    /**
     * @brief Drop queued messages that have outlived their expiry interval.
     *
     * Expiry is otherwise only noticed on the way out to a socket, so a session with no client
     * attached keeps expired messages queued - and, when persistent, stored - until it is
     * removed. Dropping the delivery here also removes its record: the record is deleted when
     * the last reference to the delivery goes.
     *
     * Re-arms itself for the next message due to expire, so a session nobody ever comes back to
     * still sheds its messages. Costs one timer per session that actually holds an expiring
     * message, and none at all for the rest.
     *
     * @param queueIsUnsent True when nothing in the queue has actually gone to a client, so
     *                      expired messages are dropped wherever they sit. False leaves the
     *                      in-flight ones alone: those are genuinely awaiting acknowledgement.
     */
    void pruneExpiredMessages(bool queueIsUnsent);

    /**
     * @brief Record an inbound QoS 2 packet id as received but not yet released.
     *
     * The id is allocated by the client, so it is unique within this session regardless of
     * which node currently owns the session. Newly recorded ids are persisted asynchronously
     * so that a node taking the session over can still recognise a repeated PUBLISH.
     *
     * @param messageId         Packet id from the client's PUBLISH.
     * @return True if the id is new and the message should be delivered. False if it repeats a
     *         PUBLISH already delivered and still awaiting PUBREL, in which case it must not be
     *         delivered a second time.
     */
    bool beginQos2Receive(const MessageId& messageId);

    /**
     * @brief Forget an inbound QoS 2 packet id once its PUBREL has been handled.
     * @param messageId         Packet id from the client's PUBREL.
     */
    void endQos2Receive(const MessageId& messageId);

    /**
     * @brief Restore inbound QoS 2 packet ids awaiting PUBREL, when a session is loaded.
     * @param messageIds        Packet ids read back from storage.
     */
    void restoreQos2ReceiveIds(const std::vector<MessageId>& messageIds);

    /**
     * @brief Accept an ACK message from the client.
     * @param messageId          The ID of the acked message.
     * @param messageType        The type of the acked message.
     */
    void receivedAck(const MessageId& messageId, Message::Type messageType) override;

    /**
     * @brief Post the message to the sending queue.
     * @param message            Message to post.
     * @param qos                QOS
     * @param subscriptionIds    Matched subscription IDs.
     * @param retain             Retain flag.
     */
    MessageId postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet& subscriptionIds, bool retain) override;

    /**
     * @brief Directly send the message to the client.
     * @param messageDispatch               Message delivery.
     */
    void sendMessage(SMessageDispatch& messageDispatch) override;

    SendReceiveResult receiveMessages() override;

    void closeSessionIfNoConnectMessageAfter(const std::chrono::milliseconds& timeout);

    [[nodiscard]] SMessage decodePacket(Packet&& packet, const uint64_t packetReceivedTS) override
    {
        return m_messageReader->readMessage(std::move(packet), *this, packetReceivedTS);
    }

    void handleMessage(const SMessage& sharedMessage) override;

    void setProtocol(const GenericProtocol& protocol) override;

    /**
     * @brief The session's write buffer, as an owning pointer.
     *
     * Returned by value so the caller keeps it alive: setProtocol() replaces it on takeover, and a
     * reference into the member would be left pointing at a destroyed buffer.
     */
    [[nodiscard]] SSocketWriteBuffer socketWriteBuffer() const
    {
        return m_socketWriteBuffer.load(std::memory_order_acquire);
    }

    /**
     * @brief Take over bytes that arrived on this session's new connection before it owned it.
     *
     * A connection is accepted with a throwaway session, and the CONNECT it carries may hand it to
     * an existing session instead. MQTT 5, 3.1.4 allows a client to send further packets without
     * waiting for the CONNACK, so whatever followed the CONNECT has already been read out of the
     * socket by that throwaway session - and the edge-triggered reactor will not signal bytes it
     * has already delivered. They are handed over here rather than parsed by their finder, which
     * would register a subscription against a session about to be discarded.
     *
     * Left for the receive thread to pick up, in the same way the takeover's buffer discard is,
     * because m_readBuffer belongs to that thread alone.
     *
     * @param bytes             Unparsed remainder of the buffer.
     */
    void adoptPendingBytes(std::shared_ptr<sptk::Buffer> bytes)
    {
        m_adoptedBytes.store(std::move(bytes), std::memory_order_release);
    }

    /**
     * @brief Puts aside what followed a CONNECT that is still waiting on an extension's answer.
     *
     * Written by the receive thread when it stops reading, taken by whoever completes the
     * authentication - which is another thread, and the reason this is not simply left in the read
     * buffer. It cannot be: if that CONNECT takes over an existing session, the socket goes with it
     * and this session is never read again, so the bytes would sit in a session nothing looks at.
     * That is a SUBSCRIBE sent in the same segment as the CONNECT, silently dropped.
     */
    void stashBytesAwaitingAuthentication(std::shared_ptr<sptk::Buffer> bytes)
    {
        m_bytesAwaitingAuthentication.store(std::move(bytes), std::memory_order_release);
    }

    /// Takes them, leaving nothing behind. Answers nullptr when the CONNECT stood alone.
    std::shared_ptr<sptk::Buffer> takeBytesAwaitingAuthentication()
    {
        return m_bytesAwaitingAuthentication.exchange(nullptr, std::memory_order_acquire);
    }

    /**
     * @brief Whether this session's CONNECT is waiting on an extension's answer.
     *
     * While it is, nothing more is read from the socket. The client may legitimately have sent a
     * SUBSCRIBE straight after its CONNECT - MQTT 5, 3.1.4 allows it - and acting on that before
     * anyone has said the client may connect at all would be acting for an unauthenticated client.
     * The bytes stay in the socket and in the read buffer; the session is queued again when the
     * answer arrives.
     */
    [[nodiscard]] bool authenticationPending() const
    {
        return m_authenticationPending.load(std::memory_order_acquire);
    }

    void setAuthenticationPending(const bool pending)
    {
        m_authenticationPending.store(pending, std::memory_order_release);
    }

    /**
     * @brief Is reading this session paused until Redis catches up with its record writes?
     *
     * Set when the next packet is a PUBLISH that would add a record while
     * persistence.max_queued_writes records are already unconfirmed. The socket is then not read
     * at all, so the client is slowed by TCP itself rather than by a receive thread parked on its
     * behalf - a parked thread stops reading every other session it serves, and their PINGREQs go
     * unread until the broker disconnects them. MessageDelivery resumes the session.
     */
    [[nodiscard]] bool awaitingWriteCapacity() const
    {
        return m_awaitingWriteCapacity.load(std::memory_order_acquire);
    }

    /// @return true if the pause starts now, false if the session was already paused.
    bool pauseForWriteCapacity()
    {
        return !m_awaitingWriteCapacity.exchange(true, std::memory_order_acq_rel);
    }

    /// Reads the session again. The pause was the broker's silence, not the client's, so the
    /// keep-alive interval starts over.
    void resumeAfterWriteCapacity();

    /**
     * @brief Record when the reactor saw this session become readable.
     *
     * The reactor hands the session to a receive thread, and only that thread stamps a timestamp
     * into the message trace - so without this the queue wait between the two is invisible, and
     * falls into the one segment of the breakdown that spans two hosts and is therefore worthless
     * (their clocks agree to milliseconds, against a measurement in microseconds). Written by the
     * reactor thread, read by the receive thread, and only while a latency trace is being taken.
     */
    void setReactorReadyTimestamp(const uint64_t timestampMcs)
    {
        m_reactorReadyTimestamp.store(timestampMcs, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t getReactorReadyTimestamp() const
    {
        return m_reactorReadyTimestamp.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isClusterSession() const
    {
        return m_isClusterSession;
    }

    void setClusterSession(const bool isClusterSession)
    {
        m_isClusterSession = isClusterSession;
    }

    [[nodiscard]] bool isClusterLink() const override
    {
        return isClusterSession();
    }

    [[nodiscard]] std::string bridgeOrigin() const override
    {
        return m_bridgeOrigin == nullptr ? std::string() : *m_bridgeOrigin;
    }

    void setBridgeOrigin(const std::string& bridgeOrigin) override
    {
        m_bridgeOrigin = SharedStringTable::instance().intern(bridgeOrigin);
    }

    SClientSession shared()
    {
        return std::dynamic_pointer_cast<ClientSession>(shared_from_this());
    }

protected:
    bool queueSendMessage(const MessageDispatch& messageDispatch) override;
    void queueMessageDelivery(SMessageDispatch& messageDispatch) const override;
    void restoreMessageDelivery(SMessageDispatch& messageDispatch) const override;

    void ackPublishMessage(const Message* message, ReasonCode reasonCode) override;
    void handleConnectMessage(const SMessage& message) override;
    void handleDisconnect(const Message* message) override;
    void handlePingReq(const Message* message) override;
    void handlePublishMessage(const SMessage& message) override;
    void handlePublishReleaseMessage(const Message* message) override;
    void handleSubscribeMessage(const Message* msg) override;
    void handleUnsubscribeMessage(const Message* msg) override;
    void autoAck(const Message* message) override;

private:
    // Serializes connection accounting across CONNECT completion and concurrent close.
    std::mutex m_accountingMutex;
    // Protected by m_mutex. Accepted TCP sockets and refused CONNECTs never set this.
    bool m_statisticsRegistered {false};
    std::atomic<std::time_t> m_connectedAt {0};
    /// Set on the path that removes the socket: whether that close named the connection it was
    /// closing. Relaxed and never read by the server itself - only the reconnect tests ask.
    std::atomic_bool m_lastCloseNamedItsConnection {true};

    /// Which call site asked for the close that took the socket away. A relaxed store of a small
    /// number, so that a failing test can name the path instead of leaving it to be guessed at.
    std::atomic_int m_lastCloseSite {0};

    /**
     * @brief Storage key of this session's inbound QoS 2 packet ids awaiting PUBREL.
     */
    [[nodiscard]] std::string qos2ReceiveKey() const;

    std::atomic<SessionState>               m_sessionState {SessionState::Accepted}; ///< What the session is waiting for.
    sptk::STimerEvent                       m_sessionTimerEvent;          ///< The session's one timer event, armed at what its state waits for.
    sptk::DateTime::time_point              m_sessionTimerTime;           ///< Time m_sessionTimerEvent is armed at; default when nothing is armed.
    sptk::DateTime::time_point              m_connectDeadline;     ///< When the CONNECT message stops being waited for; default once it has arrived.
    sptk::DateTime::time_point              m_sessionExpirationDeadline;  ///< When a session with no client is removed; default when it does not expire.
    sptk::DateTime::time_point              m_messageExpirationDeadline;  ///< When the next queued message expires; default when nothing queued expires.
    std::chrono::milliseconds               m_waitForConnectTimeout {0};  ///< The timeout the CONNECT deadline was made from, for the message logged when it passes.
    std::unordered_set<MessageId>           m_unreleasedQos2Ids;          ///< Inbound QoS 2 packet ids delivered but awaiting PUBREL.
    std::atomic_bool                        m_discardReadBuffer {false};  ///< Set by a takeover, honoured by the receive thread.
    PacketReader*                           m_packetReader {nullptr};     ///< Packet reader.
    MessageReader*                          m_messageReader {nullptr};    ///< Message reader.
    MessageWriter*                          m_messageWriter {nullptr};    ///< Message writer.
    AtomicSharedPtr<SocketWriteBuffer>      m_socketWriteBuffer;          ///< Socket write buffer; replaced on takeover.
    sptk::Buffer                            m_readBuffer;                 ///< Per-session inbound read buffer (drained in batches.)
    AtomicSharedPtr<sptk::Buffer>              m_adoptedBytes;            ///< Bytes handed over by a takeover; see adoptPendingBytes().
    AtomicSharedPtr<sptk::Buffer>              m_bytesAwaitingAuthentication; ///< What followed a CONNECT an extension has not answered yet.
    std::atomic_bool                        m_isClusterSession {false};   ///< Is this session a cluster control session?
    std::atomic<uint64_t>                   m_reactorReadyTimestamp {0};  ///< When the reactor last saw this session readable; see setReactorReadyTimestamp().
    std::atomic_bool                        m_authenticationPending {false}; ///< CONNECT is waiting on an extension; see authenticationPending().
    std::atomic_bool                        m_awaitingWriteCapacity {false}; ///< Reading paused for Redis; see awaitingWriteCapacity().
    const std::string*                      m_bridgeOrigin {nullptr};     ///< What bridge this session is connected to.
    bool                                    m_logPublishMessages;         ///< Is logging of Publish messages enabled?
    static ClientSessionSerial              m_serial;                     ///< Session serial.

    void handleConnectionHangup() const;
    /**
     * @brief Send every retained message matching one subscription, honouring its retain handling.
     *
     * Wildcard and exact subscriptions go through the same path: the retained messages are held
     * per topic, so a filter simply matches more of them.
     */
    void postRetainedMessages(const std::shared_ptr<Subscription>& subscription,
                              SessionSubscription*                 connectionSubscription,
                              bool                                 subscriptionExisted);

    bool initializeSessionContinuation(const SConnectMessage& connectMessage);

    /// A subscription made by SUBSCRIBE, and whether the session already held it - which is what
    /// retain handling 1 decides on.
    struct NewSubscription
    {
        std::shared_ptr<Subscription> subscription;
        bool                          existed;
    };

    void sendRetainedMessages(const std::vector<NewSubscription>& subscriptions);
    /**
     * @brief Directly send a message to the client.
     * @param messageDispatch               Message delivery.
     */
    /// The queue hands a message back when its turn comes; this session writes it to the socket.
    void forwardMessage(const SMessageDispatch& messageDispatch) override
    {
        sendMessageUnlocked(*messageDispatch);
    }

    void sendMessageUnlocked(const MessageDispatch& messageDispatch);
    void clearSessionUnlocked();
};

/**
 * @brief Client connection shared pointer.
 */
using SClientSession = std::shared_ptr<ClientSession>;

} // namespace xmq
