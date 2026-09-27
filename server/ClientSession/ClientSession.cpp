/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review          2026/02/06                                             ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "ClientSession.h"
#include "Server.h"
#include "base/LatencyTrace.h"
#include "base/ProtocolException.h"
#include "common/SubscribeAckMessage.h"
#include "common/mqtt/PublishMessage.h"
#include <algorithm>
#include <format>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

#ifdef min
#undef min
#endif

namespace {
// A CONNACK with its properties fits; anything larger grows the buffer, which then keeps what it grew to.
constexpr size_t initialWriteBufferSize = 128;

// A deadline the session is not waiting on. The session's timer is armed at the earliest of the
// deadlines that are not this.
constexpr sptk::DateTime::time_point noDeadline {};
} // namespace

ClientSessionSerial ClientSession::m_serial;

ClientSession::ClientSession(Server*                          server,
                             const SConnectMessageParameters& connectMessageParameters,
                             const SMessageProperties&        connectMessageProperties)
    : ClientConnection(server, server->getTopicManager(), connectMessageParameters, connectMessageProperties)
{
    m_logPublishMessages = server->getSettings()->logSubjectEnabled(LogSubject::Publish, LogPriority::Debug);

    int64_t maxInflightMessages = 32768;
    if (connectMessageProperties)
    {
        connectMessageProperties->getProperty(Property::ReceiveMaximum, maxInflightMessages);
    }
    setInflightLimit(static_cast<uint16_t>(maxInflightMessages));

    if (connectMessageProperties)
    {
        int64_t maxPacketSize = 0;
        connectMessageProperties->getProperty(Property::MaximumPacketSize, maxPacketSize);
        setMaximumPacketSize(maxPacketSize);
    }
}

std::shared_ptr<ClientSession> ClientSession::factory(Server*                          server,
                                                      const SConnectMessageParameters& connectMessageParameters,
                                                      const SMessageProperties&        connectMessageProperties)
{
    auto session = make_shared<ClientSession>(server, connectMessageParameters, connectMessageProperties);
    if (!connectMessageParameters->getClientId().empty() && connectMessageParameters->m_cleanSession == 0)
    {
        session->persist();
    }
    return session;
}

ClientSession::~ClientSession() noexcept
{
    const unique_lock lock(m_mutex);

    m_sessionState.store(SessionState::Gone, std::memory_order_relaxed);
    if (m_sessionTimerEvent)
    {
        // Clears the callback, and with it the weak pointer it holds. The event itself is left for
        // the timer to drop when it reaches the head of the queue: searching the queue for it would
        // cost more than the wait.
        m_sessionTimerEvent->cancel();
    }

    if (isCleanSession())
    {
        unsubscribeAllUnlocked();
    }
}

void ClientSession::loadSession(const std::string& /*clientId*/)
{
    // Not yet implemented.
}

void ClientSession::clearSession(const SConnectMessage& connectMessage)
{
    const unique_lock lock(m_mutex);

    // TODO: Decide if remove persistent data
    // removePersistentData();

    clearSessionUnlocked();

    applyConnectParametersUnlocked(connectMessage->getParameters());
    setConnectPropertiesUnlocked(connectMessage->getProperties());
}

void ClientSession::clearSession()
{
    const unique_lock lock(m_mutex);
    clearSessionUnlocked();
}

/**
 * Clears the session without acquiring a lock.
 *
 * This function is used to clear the session when the lock is already held.
 * It cancels any pending session expiration events, removes existing subscriptions,
 * and clears messages queued for delivery.
 */
void ClientSession::clearSessionUnlocked()
{
    // The session is not waiting to expire any more. The timer event stays as it is: it will fire,
    // find nothing due, and arm itself at whatever this session waits on next.
    m_sessionExpirationDeadline = noDeadline;

    // Remove existing subscriptions to prevent further message delivery
    unsubscribeAllUnlocked();

    // Clean messages queued for delivery to prevent stale messages
    if (const auto& queue = getInflightQueueUnlocked())
    {
        queue->clear();
    }
}

void ClientSession::continueSession(const SConnectMessage& connectMessage)
{
    if (const auto isCleanSession = initializeSessionContinuation(connectMessage);
        !isCleanSession)
    {
        // Before anything is sent: a message that expired while the session was away must not be
        // delivered late, and its record should not survive the reconnect either. Nothing queued
        // has gone out yet - what was in flight was put back to waiting when the session closed,
        // and what arrived since was queued to a send that did nothing.
        pruneExpiredMessages(true);

        const unique_lock lock(m_mutex);
        if (const auto& queue = getInflightQueueUnlocked())
        {
            queue->sendWaitingMessages(
                [this](const SMessageDispatch& messageDispatch)
                {
                    sendMessageUnlocked(*messageDispatch);
                });
        }
    }
    sendRetainedMessages();
    scheduleIdleDisconnect();
}

bool ClientSession::initializeSessionContinuation(const SConnectMessage& connectMessage)
{
    const unique_lock lock(m_mutex);

    // The client is back, so the session no longer expires.
    m_sessionExpirationDeadline = noDeadline;

    applyConnectParametersUnlocked(connectMessage->getParameters());
    setConnectPropertiesUnlocked(connectMessage->getProperties());

    return isCleanSession();
}

void ClientSession::closeSession()
{
    (void) closeSession({});
}

bool ClientSession::closeSession(const std::shared_ptr<ServerConnectionExt>& expectedConnection)
{
    auto notifyHangup = false;

    // Taken here rather than after the lock, because clearConnection() below takes the connection -
    // and with it the address - away. Empty client id means there was nothing to disconnect.
    std::string disconnectedClientId;
    std::string disconnectedUsername;
    std::string disconnectedAddress;

    {
        const unique_lock lock(m_mutex);

        if (expectedConnection && getConnection() != expectedConnection)
        {
            // Overtaken by a takeover: the session has moved to a connection this close was never
            // about. Tearing it down here would drop the socket the reconnect is using.
            //
            // Said out loud, because the caller has bookkeeping of its own to skip. It used to run
            // that regardless, and a takeover landing between its own check and this one then took
            // the session out of the manager - the session the new connection was already using.
            return false;
        }

        // Nothing to keep alive without a client. The keep-alive deadline is read from the session
        // rather than stored, so there is nothing to clear but the session's connected state, which
        // the close below takes care of.

        if (isConnected())
        {
            BaseClientSession::closeSession();

            if (isCleanSession())
            {
                if (const auto& queue = getInflightQueueUnlocked())
                {
                    queue->clear();
                }
                clearSessionUnlocked();
            }
            else
            {
                if (const auto& queue = getInflightQueueUnlocked())
                {
                    queue->rescheduleMessagesWaitingForAck();
                }
            }

            // Read before the connection is dropped: the hangup state lives on the connection, and
            // clearConnection() takes the answer with it. Getting this order wrong silently stops
            // the last will from ever being published.
            notifyHangup = isHangup();

            // Every way a session ends passes through here - a DISCONNECT packet, a lost socket, a
            // keep-alive that expired, a takeover, the broker stopping - which is why the event is
            // raised here and not in the handler for one of them. It used to be raised only for the
            // DISCONNECT packet, so an observer saw sessions that began and never ended.
            disconnectedClientId = getClientIdUnlocked();
            disconnectedUsername = getUsername();
            if (const auto& connection = getConnection())
            {
                disconnectedAddress = std::string(connection->address());
            }

            m_lastCloseNamedItsConnection.store(expectedConnection != nullptr, std::memory_order_relaxed);
            clearConnection();
        }

        scheduleExpiration();
        setSessionStateUnlocked(isCleanSession() ? SessionState::Gone : SessionState::Detached);
    }

    // server() locks, so this waits for the same release handleConnectionHangup() does.
    if (!disconnectedClientId.empty())
    {
        server().extensions().publishEvent(XMQ_EVENT_CLIENT_DISCONNECTED, disconnectedClientId,
                                           disconnectedUsername, {}, 0, 0, false,
                                           {{ExtensionHost::remoteAddressAttribute, disconnectedAddress}});
    }

    // handleConnectionHangup() uses the locking accessors (server(), prefix(), getLastWillMessage()),
    // so it must be called after m_mutex is released - shared_mutex is not recursive.
    if (notifyHangup)
    {
        handleConnectionHangup();
    }

    return true;
}

void ClientSession::postRetainedMessages(const std::shared_ptr<Subscription>& subscription,
                                         SessionSubscription*                 connectionSubscription)
{
    if (connectionSubscription == nullptr)
    {
        return;
    }

    const auto subscriptionOptions = connectionSubscription->options();

    const auto retainHandling = static_cast<SubscribeRetainHandling>(subscriptionOptions.m_retainHandling);
    if (retainHandling == SubscribeRetainHandling::DoNotRetain)
    {
        return;
    }
    if (retainHandling != SubscribeRetainHandling::RetainAlways && connectionSubscription->retainDelivered())
    {
        return;
    }

    // One retained message per matching topic, and the topic is the one it was published to. A
    // wildcard subscriber used to get a single message named after its own filter, because the
    // payload was kept on the subscription rather than on the topic.
    auto delivered = false;
    serverUnlocked().getSubscriptionManager()->retainedMessages().forEachMatching(
        subscription->fullName(),
        [this, &subscriptionOptions, &delivered](const std::string& topicName, const RetainedMessages::Record& record)
        {
            auto retainedMessage = make_shared<mqtt::PublishMessage>(serverUnlocked().getTopic(topicName), record.m_payload,
                                                                     static_cast<MessageId>(0), false);
            postMessage(retainedMessage, record.m_qos, {}, subscriptionOptions.m_retainAsPublished);
            delivered = true;
        });

    if (delivered)
    {
        connectionSubscription->setRetainDelivered(true);
    }
}

void ClientSession::sendRetainedMessages()
{
    const unique_lock lock(m_mutex);

    for (const auto& [topic, clientSubscription]: getSubscribedToUnlocked())
    {
        postRetainedMessages(clientSubscription.subscription,
                             clientSubscription.subscription->sessionSubscription(shared()));
    }
}

DateTime::time_point ClientSession::keepAliveDeadlineUnlocked() const
{
    const auto keepAliveSeconds = getKeepAliveSeconds();
    if (keepAliveSeconds <= 0)
    {
        // Keep-alive 0 switches the mechanism off, which MQTT allows: the session then waits for
        // nothing while it is connected.
        return noDeadline;
    }
    constexpr auto oneAndHalfMultiplier = 1500;
    return getLastClientMessageTimestamp() + chrono::milliseconds(keepAliveSeconds * oneAndHalfMultiplier);
}

DateTime::time_point ClientSession::deadlineForStateUnlocked() const
{
    switch (m_sessionState.load(std::memory_order_relaxed))
    {
        using enum SessionState;

        case Accepted:
        case Authenticating:
            // The connection has a reasonable amount of time to get as far as an accepted CONNECT,
            // which is what MQTT asks a server to allow before closing it.
            return m_connectDeadline;

        case Active:
            // The client's own deadline, and its next message moves it.
            return keepAliveDeadlineUnlocked();

        case Detached:
            // Nobody is coming back for it, unless somebody does.
            return m_sessionExpirationDeadline;

        case Gone:
            return noDeadline;
    }
    return noDeadline;
}

DateTime::time_point ClientSession::armingDeadlineUnlocked() const
{
    // Two clocks, and only one of them belongs to the session. A queued message expires on its own
    // schedule whatever the session is doing - connected, detached, still being authenticated - so
    // the queue is not a state and does not belong in the switch above. It shares the event because
    // one event costs less than two, and because a session that has nothing queued that can expire
    // - which is nearly all of them - never notices the difference.
    const auto state = deadlineForStateUnlocked();
    if (state == noDeadline)
    {
        return m_messageExpirationDeadline;
    }
    if (m_messageExpirationDeadline == noDeadline)
    {
        return state;
    }
    return std::min(state, m_messageExpirationDeadline);
}

void ClientSession::setSessionStateUnlocked(const SessionState newState)
{
    m_sessionState.store(newState, std::memory_order_relaxed);
    armSessionTimerUnlocked();
}

void ClientSession::armSessionTimerUnlocked()
{
    const auto deadline = armingDeadlineUnlocked();

    if (deadline == noDeadline)
    {
        if (m_sessionTimerEvent)
        {
            m_sessionTimerEvent->cancel();
            m_sessionTimerEvent.reset();
            m_sessionTimerTime = noDeadline;
        }
        return;
    }

    if (m_sessionTimerTime != noDeadline && m_sessionTimerTime <= deadline)
    {
        // Something is armed already and fires no later than this is due, so leave it: when it
        // fires it will find nothing due and arm itself at whatever is next by then. This is the
        // ordinary case - a deadline moving further away, which is what every client message does
        // to the keep-alive - and it costs nothing here at all.
        return;
    }

    if (m_sessionTimerEvent && m_sessionTimerTime != noDeadline)
    {
        // Still in the timer's queue and moving closer, so what is out there would fire too late
        // to be of any use. An event that has already fired is not in the queue and needs nothing.
        m_sessionTimerEvent->cancel();
    }

    if (const auto timer = serverUnlocked().getTimer())
    {
        // The callback captures the weak pointer and nothing besides. That still allocates - a
        // libstdc++ std::function only stores a callable inside itself when it is trivially
        // copyable, which a captured weak_ptr is not, whatever its size - but it allocates 16 bytes
        // where the wait-for-CONNECT callback used to capture 40, and, which is the point, an event
        // left in the queue no longer holds the connection and its socket until its time comes.
        m_sessionTimerEvent = timer->fireAt(deadline,
                                            [weakSessionPtr = weak_from_this()]
                                            {
                                                if (const auto base = weakSessionPtr.lock())
                                                {
                                                    dynamic_pointer_cast<ClientSession>(base)->onSessionTimerEvent();
                                                }
                                            });
        m_sessionTimerTime = deadline;
    }
}

void ClientSession::onSessionTimerEvent()
{
    const auto now = DateTime::clock::now();

    auto connectTimeout = chrono::milliseconds {0};
    bool connectTimedOut = false;
    bool keepAliveTimedOut = false;
    bool sessionExpired = false;
    bool messagesExpired = false;

    {
        const unique_lock lock(m_mutex);

        // The timer takes an event out of its queue before firing it, so nothing is armed for this
        // session until the lines below arm it again.
        m_sessionTimerTime = noDeadline;

        const auto passed = [&now](const DateTime::time_point& deadline)
        {
            return deadline != noDeadline && deadline <= now;
        };

        switch (m_sessionState.load(std::memory_order_relaxed))
        {
            using enum SessionState;

            case Accepted:
            case Authenticating:
                connectTimedOut = passed(m_connectDeadline);
                connectTimeout = m_waitForConnectTimeout;
                break;

            case Active:
                keepAliveTimedOut = passed(keepAliveDeadlineUnlocked());
                break;

            case Detached:
                sessionExpired = passed(m_sessionExpirationDeadline);
                break;

            case Gone:
                break;
        }

        // Outside the switch, because the queue's clock runs whatever the session is doing.
        messagesExpired = passed(m_messageExpirationDeadline);

        if (!connectTimedOut && !keepAliveTimedOut && !sessionExpired && !messagesExpired)
        {
            // Either what this state waits for has not happened yet, or it stopped mattering while
            // the event was on its way here - a CONNECT that arrived in time, a client that came
            // back before its session expired.
            armSessionTimerUnlocked();
            return;
        }

        if (connectTimedOut)
        {
            m_connectDeadline = noDeadline;
        }
        if (sessionExpired)
        {
            m_sessionExpirationDeadline = noDeadline;
        }
        if (messagesExpired)
        {
            m_messageExpirationDeadline = noDeadline;
        }
    }

    // Outside the lock from here: each of these takes locks of its own, and the first three end the
    // session, which takes every lock there is.
    if (connectTimedOut)
    {
        const auto   connection = getConnection();
        const String address = connection ? connection->address() : String("unknown");
        server().logMessage(LogSubject::ServerConnections, LogPriority::Error,
                            [&address, connectTimeout]
                            {
                                return format("Connection from {} didn't send the CONNECT message within {} ms and was terminated.",
                                              address.c_str(),
                                              connectTimeout.count());
                            });
        server().unwatchSession(shared());
        noteCloseSite(4);
        closeSession();
        return;
    }

    if (keepAliveTimedOut)
    {
        server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                            [clientId = getClientId()]
                            {
                                return format("{} disconnected as keep alive timeout reached.", clientId);
                            });
        noteCloseSite(2);
        server().closeSession(shared());
        return;
    }

    if (sessionExpired)
    {
        server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                            [clientId = getClientId()]
                            {
                                return format("{}: session expired.", clientId);
                            });
        server().removeClientSession(shared());
        return;
    }

    // Drops what has expired, works out when the next message does, and arms the timer again.
    pruneExpiredMessages(!isConnected());
}

void ClientSession::scheduleIdleDisconnect()
{
    if (getKeepAliveSeconds() > 0)
    {
        touchLastClientMessageTimestamp();
    }

    // Both ways into this state come through here: a CONNECT that was accepted, and a client that
    // came back to a session that was waiting for it. The state changes even when keep-alive is
    // switched off, and then the session simply waits for nothing.
    const unique_lock lock(m_mutex);
    m_connectDeadline = noDeadline;
    setSessionStateUnlocked(SessionState::Active);
}

void ClientSession::scheduleExpiration()
{
    if (!isCleanSession())
    {
        int64_t seconds = 0;
        if (const auto& connectProperties = getConnectPropertiesUnlocked();
            connectProperties && connectProperties->getProperty(Property::SessionExpiryInterval, seconds) && seconds > 0)
        {
            // Only the deadline: the caller moves the session to the state that waits for it, and
            // that is what arms the timer.
            m_sessionExpirationDeadline = DateTime::clock::now() + chrono::seconds(seconds);
        }
    }
}

void ClientSession::pruneExpiredMessages(const bool queueIsUnsent)
{
    size_t droppedCount = 0;

    {
        uint32_t          secondsUntilNextExpiration = 0;
        const unique_lock lock(m_mutex);

        // What the session was waiting for is about to be recomputed from the queue as it is now.
        m_messageExpirationDeadline = noDeadline;

        const auto inflightQueue = getInflightQueueUnlocked();
        if (!inflightQueue)
        {
            // Arm anyway: this can be reached from the session's own timer event, and leaving
            // without arming would leave the session with no timer at all - no keep-alive, and
            // no expiry either.
            armSessionTimerUnlocked();
            return;
        }

        droppedCount = inflightQueue->removeExpiredMessages(secondsUntilNextExpiration, queueIsUnsent);

        if (secondsUntilNextExpiration != 0)
        {
            m_messageExpirationDeadline = DateTime::clock::now() + chrono::seconds(secondsUntilNextExpiration);
        }
        armSessionTimerUnlocked();
    }

    if (droppedCount != 0)
    {
        server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                            [clientId = getClientId(), droppedCount]
                            {
                                return format("{}: dropped {} expired message(s).", clientId, droppedCount);
                            });
    }
}

void ClientSession::sendMessage(SMessageDispatch& messageDispatch)
{
    const unique_lock lock(m_mutex);
    sendMessageUnlocked(*messageDispatch);
}

void ClientSession::sendMessageUnlocked(const MessageDispatch& messageDispatch)
// NOLINTNEXTLINE
{
    if (!isConnected())
    {
        return;
    }

    auto& message = messageDispatch.m_message;

    uint32_t remainingExpirationSeconds = 0;
    if (message->isExpired(remainingExpirationSeconds))
    {
        return;
    }

    if (message->is(Message::Type::Publish))
    {
        Latency::SNAP_LATENCY(message->getProperties(), LatencyPhase::ServerWireOut);
    }

    const auto writeBuffer = socketWriteBuffer();
    const auto appended = writeBuffer->appendMessageToBuffer(*message, messageDispatch.m_flags, messageDispatch.m_deliveryId,
                                                             remainingExpirationSeconds, messageDispatch.m_subscriptionIds, getMaximumPacketSize());

    if (appended && message->is(Message::Type::Publish))
    {
        incrementPublishSendCount();
    }
}

MessageId ClientSession::postMessage(const SMessage& message, const Qos qos, const SubscriptionIdSet& subscriptionIds, const bool retain)
{
    const auto& inflightQueue = inflightQueueUnlocked();

    // Allocated up front, from the queue that owns this session's packet id space, because
    // MessageDelivery::create() persists the record before the message is ever queued. An id
    // assigned afterwards would leave the record stored under a different id than the one sent to
    // the client, and an acknowledgement arriving after the session moved could not be matched.
    const MessageId deliveryId = (qos != Qos::Qos0 && inflightQueue) ? inflightQueue->nextDeliveryId() : 0;

    const auto messageDelivery = MessageDelivery::create(shared(), message, qos, deliveryId, subscriptionIds, retain);

    if (inflightQueue)
    {
        if (message->is(Message::Type::Publish))
        {
            incrementPublishScheduledCount();
        }

        inflightQueue->queueMessage(messageDelivery);
    }

    // Queued for a session with no client attached, nothing will look at it again until the
    // client returns - which it may never do. If it can expire, arm the timer that drops it.
    // Queued for a session with no client attached, nothing will look at it again until the
    // client returns - which it may never do. Any message that can expire arms the timer that
    // drops it, including one that is already past its interval on arrival.
    if (int64_t expiryInterval = 0;
        !isConnected() && message->getProperties() &&
        message->getProperties()->getProperty(Property::MessageExpiryInterval, expiryInterval) && expiryInterval != 0)
    {
        pruneExpiredMessages(true);
    }

    return deliveryId;
}

void ClientSession::queueMessageDelivery(SMessageDispatch& messageDispatch) const
{
    inflightQueueUnlocked()->queueMessage(messageDispatch);
}

void ClientSession::restoreMessageDelivery(SMessageDispatch& messageDispatch) const
{
    // A record that outlived its expiry while the server was down is not worth restoring: it can
    // never be delivered. Dropping the delivery here removes its record with it, so an expired
    // message is not reloaded at every subsequent restart.
    if (uint32_t remainingSeconds = 0;
        messageDispatch->m_message->isExpired(remainingSeconds))
    {
        return;
    }

    // Restoring puts a message into the queue, so this is one of the places that makes it: a
    // session read back from storage has messages before it has anything else.
    inflightQueueUnlocked()->restoreMessage(messageDispatch);
}

void ClientSession::receivedAck(const MessageId& messageId, const Message::Type messageType)
{
    if (const auto inflightQueue = getInflightQueueUnlocked())
    {
        inflightQueue->receiveAck(messageId, messageType);
    }
}

SendReceiveResult ClientSession::receiveMessages()
{
    SendReceiveResult result {};

    if (authenticationPending())
    {
        // An extension has not yet said whether this client may connect. Its bytes stay in the
        // socket until it has; the session is queued again from the answer.
        result.noMoreMessages = true;
        return result;
    }

    // Held for the duration: a raw pointer taken from the returned shared_ptr would outlive
    // it, and a concurrent disconnect could then destroy the socket under this thread.
    // Honoured here rather than where it was requested: this is the only thread that owns
    // m_readBuffer, so a takeover asks for the discard and it happens before the first bytes of
    // the new connection are read.
    if (m_discardReadBuffer.exchange(false, std::memory_order_acquire))
    {
        m_readBuffer.bytes(0);
    }

    // After the discard, never before: these bytes arrived on the connection this session has just
    // been handed, so the discard that comes with a takeover must not eat them. See
    // adoptPendingBytes() for why another session has them to give.
    if (const auto adopted = m_adoptedBytes.exchange(nullptr, std::memory_order_acquire))
    {
        m_readBuffer.append(adopted->data(), adopted->bytes());
    }

    // Whose bytes this pass is reading. A CONNECT in them can hand the connection to a different
    // session part-way through the buffer, and what follows it then belongs to that session.
    const auto connection = getConnection();

    const auto socket = getSocket();
    if (!socket)
    {
        result.noMoreMessages = true;
        return result;
    }

    try
    {
        const auto packetReceivedTS = LatencyTrace::now();

        // Drain the socket into the per-session read buffer with a single recv, then
        // parse every complete packet out of it. The recv is capped, so bytes may remain:
        // in OneShot mode the re-arm re-checks readiness and re-fires the event for them;
        // in EdgeTriggered mode they never re-fire on their own, so the caller must take
        // another pass while noMoreMessages is false.
        //
        // One syscall, not two. This used to ask FIONREAD for the exact size and read only when
        // the answer was above zero - so a pass that found nothing still paid for asking.
        // Measured at 250k messages a second: 112659 ioctls against 91179 reads they sized, while
        // syscall entry and exit alone was a fifth of the broker's profile. readAvailable() reads
        // what is there and says RECV_RETRY when that is nothing, without read()'s 500ms wait.
        //
        // The scratch buffer belongs to the receive thread, not to the session: 100000 sessions
        // cannot each hold a read-sized buffer, and a 256Kb per-session reserve was tried and
        // reverted for exactly that reason.
        constexpr size_t           maxChunkBytes = 256u * 1024uL;
        static thread_local Buffer scratch(maxChunkBytes);
        if (const auto received = socket->readAvailable(scratch.data(), maxChunkBytes);
            received == sptk::Socket::RECV_RETRY || received == 0)
        {
            // Nothing waiting, or the peer closed - the reactor reports the hangup and the
            // teardown happens there. Either way this pass has no more to read.
            result.noMoreMessages = true;
        }
        else
        {
            m_readBuffer.append(scratch.data(), received);
            // A short read means a plain socket is drained: recv() hands over everything the
            // kernel holds. It proves nothing on a TLS one, which returns a single record however
            // large the buffer is, and leaves the rest in the kernel where EdgeTriggered epoll
            // will not announce it again - it only fires on arrival, and those bytes arrived
            // once already. Stopping there deadlocks a client that has filled its send buffer
            // and is waiting for the broker to read, against a broker waiting for an event that
            // cannot come: measured on 2026-09-08 as 4 messages of 500 delivered, with 4404
            // bytes sitting in the socket's receive queue. So a TLS socket is read until it
            // answers RECV_RETRY, which is what the branch above tests.
            result.noMoreMessages = received < maxChunkBytes && !socket->readsInRecords();
        }

        if (m_readBuffer.bytes() == 0)
        {
            result.noMoreMessages = true;
            return result;
        }

        // This session no longer owns the connection, so nothing left in its buffer is addressed to
        // it: a CONNECT it handled took the socket over to another session. What follows belongs
        // there, and parsing it here would act on a session that is about to be discarded.
        //
        // The handover below in the CONNECT branch does this for a synchronous connect, which
        // finishes the takeover inside this very loop. An authenticated one does not: the loop
        // breaks at authenticationPending() long before the takeover happens, and the remainder is
        // still sitting here when the answer finally comes. That is a SUBSCRIBE pipelined behind a
        // CONNECT, and it was never answered.
        size_t offset = 0;
        while (offset < m_readBuffer.bytes())
        {
            auto empty = true;
            auto packet = m_packetReader->readPacket(m_readBuffer, empty, offset);
            if (empty)
            {
                // Only a partial packet remains; keep it for the next read.
                break;
            }

            result.bytes += packet.fullSize();
            const auto packetSize = packet.fullSize();
            if (const auto message = decodePacket(std::move(packet), packetReceivedTS))
            {
                // Carries the reactor's own timestamp into the trace, so the wait between the
                // reactor seeing this session readable and the receive thread reading it becomes
                // visible - it is the one part of the broker's path nothing else measures.
                Latency::SET_LATENCY(message->getProperties(), LatencyPhase::ServerEventReady,
                                     getReactorReadyTimestamp());
                result.anyMessages += 1;
                result.publishMessages += message->is(Message::Type::Publish) ? 1 : 0;
                handleMessage(message);

                // A CONNECT for a client id the broker already knows moves the connection to the
                // session that owns that id, and this session - created when the connection was
                // accepted - is finished with it. MQTT 5, 3.1.4 lets a client send packets without
                // waiting for the CONNACK, so the rest of this buffer can already hold its
                // SUBSCRIBE, and handling it here would register the subscription against a session
                // that is about to be thrown away. Nothing would ever answer it: the bytes are out
                // of the socket, so the edge-triggered reactor has no further event to give.
                if (authenticationPending())
                {
                    // The CONNECT just handled went to an extension and has no answer yet. What
                    // follows it in this buffer belongs to a client nobody has admitted, and is
                    // left where it is - the loop below keeps the unconsumed remainder.
                    break;
                }

                if (message->is(Message::Type::Connect))
                {
                    if (auto owner = connection ? connection->getClientSession() : nullptr;
                        owner && owner.get() != this)
                    {
                        if (offset < m_readBuffer.bytes())
                        {
                            auto pending = std::make_shared<Buffer>();
                            pending->append(m_readBuffer.data() + offset, m_readBuffer.bytes() - offset);
                            owner->adoptPendingBytes(std::move(pending));
                            owner->clientSessionReceiveThread()->queueProcessSession(owner);
                        }
                        m_readBuffer.bytes(0);
                        result.noMoreMessages = true;
                        return result;
                    }
                }
            }
            else
            {
                // A packet that was read but did not decode is skipped without a word, so a
                // client that sent something perfectly valid sees it accepted and answered by
                // nothing at all.
                server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                                    [this, packetSize]
                                    {
                                        return getClientId() + ": a " + to_string(packetSize) +
                                               " byte packet was read but did not decode - dropped.";
                                    });
            }
        }

        // Whatever followed a CONNECT that is still being authenticated is put aside rather than
        // kept here. Kept here it would be lost: if that CONNECT takes over an existing session the
        // socket goes with it, this session is never read again, and nothing would ever look at
        // this buffer. Whoever completes the authentication hands these bytes to the session that
        // ends up owning the connection - which is this one when nothing was taken over.
        if (authenticationPending() && offset < m_readBuffer.bytes())
        {
            auto awaiting = std::make_shared<Buffer>();
            awaiting->append(m_readBuffer.data() + offset, m_readBuffer.bytes() - offset);
            stashBytesAwaitingAuthentication(std::move(awaiting));
            m_readBuffer.bytes(0);
            return result;
        }

        // Drop consumed bytes; keep any partial-frame remainder for the next call.
        if (offset >= m_readBuffer.bytes())
        {
            m_readBuffer.bytes(0);
        }
        else if (offset > 0)
        {
            m_readBuffer.erase(0, offset);
        }

        touchLastClientMessageTimestamp();
    }
    catch (const ConnectionException&)
    {
        // The clientSession hanged up.
        //
        // Named, and it has to be. A graceful DISCONNECT is followed by the socket reaching end of
        // file, so this arrives second, and by then a reconnect on the same client id may already
        // have handed the session its replacement. Closing without saying which connection this is
        // about makes Server::closeSession() ask the session - which answers with the new one, and
        // that is what gets closed. The client is then left holding a connection the broker has
        // stopped reading: its CONNECT was answered, so it believes it is connected, and its
        // SUBSCRIBE is never acknowledged.
        noteCloseSite(1);
        server().closeSession(shared(), false, connection);
        handleConnectionHangup();
        result.noMoreMessages = true;
    }
    catch (const ProtocolException& exception)
    {
        disconnect(exception.getReasonCode(), exception.message());
        result.noMoreMessages = true;
    }
    catch (const Exception& e)
    {
        auto reason = e.what();
        disconnect(ReasonCode::ProtocolError, reason);
        server().logMessage(LogSubject::SessionErrors, LogPriority::Error,
                            [reason]
                            {
                                return reason;
                            });
        result.noMoreMessages = true;
    }

    // Can't read more messages
    return result;
}

void ClientSession::handleMessage(const SMessage& sharedMessage)
{
    const Message* msg = sharedMessage.get();

    if (!msg)
    {
        // Continue reading messages
        return;
    }

    // If the session didn't receive Connect yet, ignore any other messages
    if (!connectMessageReceived() && msg->type() != Message::Type::Connect)
    {
        // Logged rather than dropped in silence: a message discarded here has been read off the
        // socket and decoded, so from the client's side it was accepted, and the connection stays
        // open. Without this line the loss leaves no trace anywhere.
        server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                            [this, msg]
                            {
                                return getClientId() + ": " + msg->toString() +
                                       " discarded - no CONNECT received on this session yet.";
                            });
        // Continue reading messages
        return;
    }

    switch (msg->type())
    {
        using enum Message::Type;
        case Connect:
            handleConnectMessage(sharedMessage);
            break;

        case Subscribe:
            handleSubscribeMessage(msg);
            break;

        case Unsubscribe:
            handleUnsubscribeMessage(msg);
            break;

        case Publish:
            if (const auto* publishMessage = dynamic_cast<const PublishMessage*>(msg))
            {
                // If message is a trace, update trace.
                if (const auto messageProperties = publishMessage->getProperties())
                {
                    Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ServerHandler);
                }

                const auto rc = Server::grantPublish(shared(), publishMessage->destination());
                if (rc == ReasonCode::Success)
                {
                    handlePublishMessage(sharedMessage);
                }
                ackPublishMessage(msg, rc);
            }
            break;

        case PublishRelease:
            handlePublishReleaseMessage(msg);
            break;

        case Disconnect:
            handleDisconnect(msg);
            break;

        case ConnectAck:
        case SubscribeAck:
            server().logMessage(LogSubject::Ack, LogPriority::Debug,
                                [prefix = prefix(), msg]
                                {
                                    return prefix + msg->toString();
                                });
            break;

        case PublishAck:
        case PublishReceived:
        case PublishComplete:
        case UnsubscribeAck:
        case PingResp:
            if (isConnected())
            {
                receivedAck(msg->getId(), msg->type());
            }
            break;

        case PingReq:
            handlePingReq(msg);
            break;

        case Undefined:
            throw Exception("Unsupported message type " + Message::messageTypeName(msg->type()));
    }
}

void ClientSession::handleConnectionHangup() const
{
    server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                        [this]
                        {
                            return prefix() + getClientId() + " hangup.";
                        });

    if (const auto lwtMessage = getLastWillMessage(server().getTopicManager()))
    {
        if (lwtMessage)
        {
            server().logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                                [prefix = prefix(), lwtMessage]
                                {
                                    return prefix + "last will message: " + lwtMessage->toString() + ".";
                                });
            server().getSubscriptionManager()->publishMessage(lwtMessage);
        }
    }
}

void ClientSession::handleConnectMessage(const SMessage& message)
{
    {
        // The packet is here, the verdict is not. The deadline does not move: a CONNECT that is
        // never answered - an authenticator that hangs, a queue that is not draining - leaves the
        // connection to be closed at the same timeout as a CONNECT that never came, which is what
        // MQTT asks of a server that cannot get as far as a CONNACK.
        const unique_lock lock(m_mutex);
        setSessionStateUnlocked(SessionState::Authenticating);
    }

    if (const auto connectMessage = dynamic_pointer_cast<ConnectMessage>(message))
    {
        const auto sharedSession = shared();
        if (const auto reasonCode = server().handleConnectMessage(sharedSession, connectMessage);
            reasonCode == ReasonCode::Success)
        {
            scheduleIdleDisconnect();
            setConnectMessageReceived(true);
        }
        else
        {
            server().logMessage(LogSubject::Connect, LogPriority::Error,
                                format("Session rejected, reason: {}.", xmq::toString(reasonCode)));
        }
    }
}

void ClientSession::ackPublishMessage(const Message* message, const ReasonCode reasonCode)
{
    incrementPublishReceiveCount();

    switch (message->getQos())
    {
        using enum Qos;
        case Qos0:
            break;
        case Qos1:
        case Qos2:
            socketWriteBuffer()->appendAckToBuffer(message, reasonCode);
            break;
        case Invalid:
            break;
    }
}

void ClientSession::handlePublishMessage(const SMessage& message)
{
    const auto publishMessage = dynamic_pointer_cast<PublishMessage>(message);

    publishMessage->setSender(getClientId());

    // A message published by a bridge's own connection is bridged traffic, and has to be marked
    // as such so Subscription::deliverTo() will not hand it to a bridge subscription and send it
    // straight back. Bridge::acceptRemoteMessage() marks the messages this server pulls in; this
    // marks the ones a remote bridge pushes at us, which is the other half. Without it a pair of
    // bridges pointing at each other echoes every message between them without end - each side
    // seeing an unmarked message and dutifully forwarding it on.
    if (const auto origin = bridgeOrigin();
        !origin.empty())
    {
        publishMessage->setSourceNode(origin);
    }

    if (m_logPublishMessages)
    {
        server().logMessage(LogSubject::Publish, LogPriority::Debug,
                            [prefix = prefix(), publishMessage]
                            {
                                stringstream str;
                                str << prefix << "Received: " << *publishMessage << ".";
                                return str.str();
                            });
    }

    if (server().extensions().watching())
    {
        // Raised where the broker accepted the message, before delivery: an observer is watching
        // what clients did, not what subscribers received. The payload is not passed on - copying
        // every message body through the queue would make observation cost more than delivery.
        server().extensions().publishEvent(XMQ_EVENT_PUBLISHED, getClientId(), getUsername(),
                                           publishMessage->destination()->fullName(),
                                           publishMessage->payload().size(),
                                           static_cast<uint8_t>(publishMessage->getQos()),
                                           publishMessage->isRetain());
    }

    if (publishMessage->getQos() == Qos::Qos2 && !beginQos2Receive(publishMessage->getId()))
    {
        // A repeat of a PUBLISH already delivered and still awaiting PUBREL. Delivering again would
        // break exactly-once, so only the PUBREC the caller sends is owed here.
        return;
    }

    if (publishMessage->destination()->isCluster())
    {
        server().acceptClusterMessage(publishMessage);
    }
    else if (server().deliversOnReceiveThread())
    {
        // Delivered here, with no hand-off. The PUBACK that follows waits for it.
        server().getMessageDeliveryThreads().deliverNow(publishMessage);
    }
    else
    {
        // Handed to the delivery pool, so the publisher's PUBACK is not gated by delivery into a slow
        // subscriber, a bridge or the store. The pool preserves per-publisher order.
        server().getMessageDeliveryThreads().publishMessage(publishMessage);
    }
}

void ClientSession::handleSubscribeMessage(const Message* msg)
{
    if (const auto* subscriptionMessage = dynamic_cast<const SubscribeMessage*>(msg))
    {
        server().logMessage(LogSubject::Subscribe, LogPriority::Debug,
                            [msg]
                            {
                                return "Received: " + msg->toString() + ".";
                            });

        if (server().extensions().watching())
        {
            for (const auto& destination: subscriptionMessage->getDestinations())
            {
                if (destination.m_topic == nullptr)
                {
                    continue;
                }
                server().extensions().publishEvent(XMQ_EVENT_SUBSCRIBED, getClientId(), getUsername(),
                                                   destination.m_topic->fullName(), 0,
                                                   destination.m_subscribeOptions.m_maxQos, false);
            }
        }

        uint32_t subscriptionId = 0;
        if (subscriptionMessage->getProperties())
        {
            subscriptionId = getSubscriptionId(subscriptionMessage);
        }

        const auto  clientSession = dynamic_pointer_cast<ClientSession>(shared());
        const auto& destinations = subscriptionMessage->getDestinations();

        vector<SSubscription> subscriptions;
        vector<uint8_t>       subscriptionResults;
        subscriptions.reserve(destinations.size());
        for (const auto& destination: destinations)
        {
            auto reasonCode = Server::grantSubscription(clientSession, destination.m_topic);
            auto subscriptionResult = reasonCode == ReasonCode::Success
                                          ? destination.m_subscribeOptions.m_maxQos
                                          : static_cast<uint8_t>(reasonCode);

            subscriptionResults.push_back(subscriptionResult);
            const auto subscription = server().subscribeClient(clientSession, destination, subscriptionId);
            subscriptions.push_back(subscription);
        }

        const auto subscriptionAck = make_shared<SubscribeAckMessage>(subscriptionMessage->getId(), subscriptionResults);

        postMessage(subscriptionAck, Qos::Qos0, {}, false);

        sendRetainedMessages(subscriptions);
    }
}

void ClientSession::sendRetainedMessages(const vector<shared_ptr<Subscription>>& subscriptions)
{
    for (const auto& subscription: subscriptions)
    {
        postRetainedMessages(subscription, subscription->sessionSubscription(shared()));
    }
}

void ClientSession::handleUnsubscribeMessage(const Message* msg)
{
    const auto* unsubscribeMessage = dynamic_cast<const UnsubscribeMessage*>(msg);
    if (!unsubscribeMessage)
    {
        throw Exception(format("Message is not unsubscribe: {}", msg->toString()));
    }

    server().logMessage(LogSubject::Unsubscribe, LogPriority::Debug, [msg]
                        {
                            return msg->toString();
                        });

    for (const auto& destination: unsubscribeMessage->destinations())
    {
        server().unsubscribeClient(this, destination);
    }

    autoAck(msg);
}

bool ClientSession::beginQos2Receive(const MessageId& messageId)
{
    {
        const unique_lock lock(m_mutex);
        if (!m_unreleasedQos2Ids.insert(messageId).second)
        {
            return false;
        }
    }

    // Persisted outside the lock, and only after the in-memory insert has won: the record exists
    // so that a node taking this session over can still tell a repeated PUBLISH from a new one.
    // It is written after delivery rather than before, deliberately - an id recorded for a message
    // that was never delivered would suppress the client's retry and lose the publish.
    if (const auto redis = getRedisConnection())
    {
        redis->setHashValueAsync(qos2ReceiveKey(), to_string(messageId), 1);
    }

    return true;
}

void ClientSession::endQos2Receive(const MessageId& messageId)
{
    {
        const unique_lock lock(m_mutex);
        if (m_unreleasedQos2Ids.erase(messageId) == 0)
        {
            // Either a repeated PUBREL, or a session that arrived here mid-handshake. Nothing to
            // forget in either case, and the caller still owes the client a PUBCOMP.
            return;
        }
    }

    if (const auto redis = getRedisConnection())
    {
        redis->deleteHashKeysAsync(qos2ReceiveKey(), {to_string(messageId)});
    }
}

void ClientSession::restoreQos2ReceiveIds(const vector<MessageId>& messageIds)
{
    const unique_lock lock(m_mutex);
    m_unreleasedQos2Ids.insert(messageIds.begin(), messageIds.end());
}

string ClientSession::qos2ReceiveKey() const
{
    return format("session_{}_qos2", getClientId());
}

void ClientSession::handlePublishReleaseMessage(const Message* message)
{
    // PUBCOMP is owed for every PUBREL, including packet ids this node has no record of. After a
    // session moves between nodes the new owner may never have seen the PUBLISH that opened the
    // handshake; withholding PUBCOMP would stall the client on an id it can never retire.
    endQos2Receive(message->getId());
    autoAck(message);
}

void ClientSession::handleDisconnect(const Message* message)
{
    if (message == nullptr)
    {
        return;
    }

    // Whose goodbye this is. Taken now, because what follows - logging, an extension's handler,
    // reading properties - is somebody else's code and takes its time, and a client that says
    // goodbye and immediately connects again can be served by a new connection before the close
    // below is reached. Closing without naming this one would then drop the replacement's socket,
    // and the client would hold a connection the broker never reads again.
    const auto disconnectConnection = getConnection();

    // Before anything else in this function, and that ordering is the whole point.
    //
    // The socket close that follows a DISCONNECT is reported as its own event, and it can be
    // dispatched to a different thread than the one reading the DISCONNECT - so the two run
    // concurrently. That thread asks whether the connection hung up and, if it did, publishes the
    // last will. Everything this function does before recording the graceful disconnect is
    // therefore a window in which the will is still set and still reachable, and on a machine
    // where the close is reported promptly it gets published: a client that said goodbye properly
    // is announced as having vanished. Logging and the extension callback below are not free -
    // an extension's handler is somebody else's code - and they used to sit inside that window.
    //
    // Clearing the will as well as the hangup flag, rather than relying on the flag alone,
    // because the two are read separately and a close reported after this point would find the
    // flag set again.
    setHangup(false);
    discardLastWill();

    server().logMessage(LogSubject::Disconnect, LogPriority::Debug,
                        [prefix = prefix(), message]
                        {
                            return prefix + message->toString();
                        });

    if (message->getProperties())
    {
        // If DISCONNECT overwrites the session expiry interval - set it in session connect properties
        if (int64_t seconds = 0;
            message->getProperties()->getProperty(Property::SessionExpiryInterval, seconds))
        {
            getConnectProperties()->setProperty(Property::SessionExpiryInterval, seconds);
        }
    }

    noteCloseSite(3);
    server().closeSession(shared(), false, disconnectConnection);
}

void ClientSession::handlePingReq(const Message* message)
{
    server().logMessage(LogSubject::Ack, LogPriority::Debug,
                        [prefix = prefix(), message]
                        {
                            return prefix + message->toString();
                        });

    autoAck(message);
}

void ClientSession::disconnect(ReasonCode reasonCode, const std::string& reasonString)
{
    const auto protocolVersion = getProtocolVersion();

    // The connection this disconnect is being written to. If writing fails, the session must only
    // be closed while it is still holding that same connection: a session that is not clean
    // outlives its connections, and by then the client may have reconnected onto a new one. Closing
    // then drops the replacement's socket, and nothing the client sends on it is ever read.
    const auto disconnectConnection = getConnection();

    const auto logPriority = reasonCode == ReasonCode::Success ? LogPriority::Debug : LogPriority::Error;
    server().logMessage(LogSubject::Ack, logPriority,
                        [prefix = prefix(), reasonString]
                        {
                            return prefix + reasonString;
                        });

    try
    {
        reasonCode = protocolVersion == ProtocolVersion::MqttV5 ? reasonCode : ReasonCode::Success;
        socketWriteBuffer()->appendDisconnectToBuffer(reasonCode);
    }
    catch (const Exception&)
    {
        // If an exception thrown here, then the connection is already broken. Named rather than
        // compared and then closed unguarded: the comparison and the close have to be one step, or
        // a takeover between them turns the close on the connection that replaced this one.
        closeSession(disconnectConnection);
    }
}

void ClientSession::autoAck(const Message* message)
{
    socketWriteBuffer()->appendAckToBuffer(message, ReasonCode::Success);
}

void ClientSession::closeSessionIfNoConnectMessageAfter(const std::chrono::milliseconds& timeout)
{
    const unique_lock lock(m_mutex);
    m_waitForConnectTimeout = timeout;
    m_connectDeadline = DateTime::clock::now() + timeout;
    setSessionStateUnlocked(SessionState::Accepted);
}

void ClientSession::setProtocol(const GenericProtocol& protocol)
{
    BaseClientSession::setProtocol(protocol);
    m_packetReader = protocol.packetReader().get();
    m_messageReader = protocol.messageReader().get();
    m_messageWriter = protocol.messageWriter().get();
    // Stored, not swapped in place: a thread already inside the previous buffer keeps it alive
    // through its own shared_ptr until it is done with it.
    //
    // Small on purpose. Every connected session holds one of these whether it ever receives a
    // message or not, and 1024 bytes of it was a fifth of an idle session's heap. It does not
    // have to be large: the send thread swaps buffers with the session instead of freeing them,
    // so a buffer that once grew for a busy flow keeps its capacity and travels on, and only a
    // session that has never needed more than a CONNACK stays this small.
    m_socketWriteBuffer.store(make_shared<SocketWriteBuffer>(this, m_messageWriter, initialWriteBufferSize), std::memory_order_release);
}

bool ClientSession::queueSendMessage(const MessageDispatch& messageDispatch)
{
    const auto& message = messageDispatch.m_message;

    uint32_t remainingExpirationSeconds = 0;
    if (message->isExpired(remainingExpirationSeconds))
    {
        return false;
    }

    return socketWriteBuffer()->appendMessageToBuffer(*message, messageDispatch.m_flags, messageDispatch.m_deliveryId,
                                                      remainingExpirationSeconds, messageDispatch.m_subscriptionIds, getMaximumPacketSize());
}
