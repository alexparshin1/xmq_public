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

#include "Session.h"
#include "base/AckMessage.h"
#include "base/LatencyTrace.h"
#include "base/MessageDispatch.h"
#include "common/ConnectAckMessage.h"
#include "common/DisconnectMessage.h"
#include "common/SessionThreads.h"
#include "common/SocketFactory.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace xmq::client;

Timer            Session::keepAliveTimer;
SessionThreads   Session::m_allClientThreads(8);
STopicManager    Session::m_topicManager = make_shared<TopicManager>();
GenericProtocols Session::m_genericProtocols(m_topicManager);

// EdgeTriggered avoids the per-event re-arm epoll_ctl(MOD) that OneShot needs after every drain.
// At 100k messages a second that was hundreds of thousands of them, from eight receive threads
// onto one epoll, serialising on the kernel's ep->mtx. Windows stays OneShot: wepoll does not
// implement EPOLLET, and SPTK throws on EdgeTriggered there.
#ifdef _WIN32
constexpr auto clientSocketEventsMode = SocketPoolTriggerMode::OneShot;
#else
constexpr auto clientSocketEventsMode = SocketPoolTriggerMode::EdgeTriggered;
#endif

SocketEvents<Session> Session::m_socketEvents(
    "Session",
    [](const weak_ptr<Session>& userData, const SocketEventType eventType)
    {
        if (const auto session = userData.lock())
        {
            if (!session->isConnected())
            {
                return;
            }
            // Edge-triggered: the registration stands, and the drain below must empty the socket
            // because nothing will report the bytes it leaves behind.
            if (eventType.m_data)
            {
                session->queueReceiveMessages();
            }
            if (eventType.m_hangup || eventType.m_error)
            {
                session->hangup(ReasonCode::ServerUnavailable);
            }
        }
    },
    100ms, clientSocketEventsMode);

Session::Session(const MessageCallback& messageCallback, const shared_ptr<Logger>& logger, const std::string& bindAddress)
    : BaseClientSession(SessionType::Client)
    , m_bindAddress(bindAddress)
    , m_logger(logger)
{
    constexpr uint16_t maxInflightMessages = 32768;
    setInflightLimit(maxInflightMessages);
    BaseClientSession::onMessage(messageCallback);
}

Session::~Session()
{
    const unique_lock lock(m_mutex);
    try
    {
        closeSocketUnlocked();
    }
    catch (...)
    {
        // Block any exceptions in dtor.
    }
}

ReasonCode Session::connect(const Host& host, const ConnectCredentials& credentials, const ProtocolVersion protocolVersion,
                            const ConnectParameters& connectParameters, const SMessageProperties& connectMessageProperties,
                            const shared_ptr<SSLKeys>& sslKeys, const MessageCallback& onConnectCompleted)
{
    {
        const unique_lock lock(m_mutex);

        // Close the session if it is connected:
        closeSocketUnlocked();

        // closeSocketUnlocked() above always leaves the state at Disconnected.
        setStateUnlocked(State::Connecting);

        m_connectProperties = connectParameters;
        // Beside the deadline and outside the lock, so the timer callback and the send path can
        // both read it without touching m_mutex.
        m_keepAlivePeriod.store(connectParameters.m_keepAliveInterval, std::memory_order_relaxed);
        m_clientId = credentials.getClientId();
        m_prefix = m_clientId + " ";
        m_username = credentials.getUsername();
    }

    ReasonCode reasonCode;
    try
    {
        reasonCode = doConnect(host, credentials, protocolVersion, connectParameters,
                               connectMessageProperties, sslKeys, onConnectCompleted);
    }
    catch (...)
    {
        // Don't leave the session in the Connecting state (isConnected() == true) on failure.
        const unique_lock lock(m_mutex);
        closeSocketUnlocked();
        throw;
    }

    if (reasonCode == ReasonCode::Success)
    {
        scheduleKeepAlive(true);
    }

    return reasonCode;
}

void Session::sendConnect(const Host& host, const ConnectCredentials& credentials,
                          const ProtocolVersion protocolVersion, const ConnectParameters& connectParameters,
                          const SMessageProperties& connectMessageProperties, const shared_ptr<SSLKeys>& sslKeys)
{
    const unique_lock lock(m_mutex);

    m_protocolVersion = protocolVersion;
    m_isHangup = false;

    const auto socket = SocketFactory::createSocket(sslKeys);

    Latency::SNAP_LATENCY(connectMessageProperties, LatencyPhase::ServerWireOut);

    socket->open(host, Socket::OpenMode::CONNECT, false,
                 connectParameters.m_connectTimeout, m_bindAddress.data());

    const auto clientConnectedTs = LatencyTrace::now();

    socket->setOption(IPPROTO_TCP, TCP_NODELAY, 1);

    setInflightLimit(connectParameters.m_maxInflightMessages);

    setSocket(socket);

    const auto& newProtocol = m_genericProtocols.getProtocol(protocolVersion);
    setProtocol(newProtocol);

    ConnectMessage connectMessage(credentials, connectParameters.m_lastWillInfo, connectParameters.m_cleanSession,
                                  protocolVersion, static_cast<uint16_t>(connectParameters.m_keepAliveInterval.count()));

    connectMessage.setProperties(connectMessageProperties);
    if (connectMessageProperties)
    {
        Latency::SET_LATENCY(connectMessageProperties, LatencyPhase::ClientConnected, clientConnectedTs);
        Latency::SNAP_LATENCY(connectMessageProperties, LatencyPhase::ClientWireOut);
    }

    Buffer& messageBuffer = writeBuffer();
    protocol().messageWriter()->writeConnect(messageBuffer, &connectMessage, connectParameters.m_tweakMessage);

    socket->write(messageBuffer);
}

ReasonCode Session::receiveConnect()
{
    const unique_lock lock(m_mutex);
    auto              reasonCode {ReasonCode::UnspecifiedError};

    const auto socket = getSocket();

    try
    {
        // For SSL connections, can read-only after several attempts
        constexpr size_t maxAttempts = 10;
        for (size_t attempts = 0; attempts < maxAttempts; ++attempts)
        {
            if (constexpr size_t minPacketSize = 2;
                socket->readyToRead(1s) &&
                socket->socketBytes() >= minPacketSize)
            {
                break;
            }
        }

        const auto packetReceivedTS = LatencyTrace::now();
        auto       packet = protocol().packetReader()->readPacket(socket.get());
        if (packet.bytes() == 0)
        {
            throw ConnectionException("Invalid response from server to CONNECT message");
        }

        const auto packetBeforeDecodeTS = LatencyTrace::now();
        const auto message = protocol().messageReader()->readMessage(std::move(packet), *this, packetReceivedTS);

        if (!message || message->type() != Message::Type::ConnectAck)
        {
            throw ConnectionException("Invalid response from server to CONNECT message");
        }

        const auto messageProperties = message->getProperties();
        if (messageProperties)
        {
            using enum LatencyPhase;
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ClientWireIn, packetReceivedTS);
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ClientBeforeDecode, packetBeforeDecodeTS);
            Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ClientDecode);
        }

        Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ClientReceive);

        executeMessageCallback(message);

        if (const auto connectAckMessage = dynamic_pointer_cast<ConnectAckMessage>(message))
        {
            reasonCode = connectAckMessage->getReasonCode();
            if (reasonCode == ReasonCode::Success)
            {
                monitorIncomingDataUnlocked(true);
                setStateUnlocked(State::Connected);
            }
            else
            {
                closeSocketUnlocked();
            }
        }
    }
    catch (...)
    {
        closeSocketUnlocked();
        throw;
    }

    return reasonCode;
}

ReasonCode Session::doConnect(const Host& host, const ConnectCredentials& credentials,
                              const ProtocolVersion protocolVersion, const ConnectParameters& connectParameters,
                              const SMessageProperties& connectMessageProperties, const shared_ptr<SSLKeys>& sslKeys,
                              const MessageCallback& onConnectCompleted)
{
    {
        // Set before CONNECT goes on the wire: once monitoring starts, the receiving thread
        // reads this member (under the same lock).
        const unique_lock lock(m_mutex);
        m_onConnectCompleted = onConnectCompleted;
    }

    sendConnect(host, credentials, protocolVersion, connectParameters, connectMessageProperties, sslKeys);

    if (onConnectCompleted != nullptr)
    {
        // Async connect
        monitorIncomingData(true);
        return ReasonCode::Success;
    }

    return receiveConnect();
}

void Session::hangup(const ReasonCode serverHangupReason, const bool abortive)
{
    const ReasonCode reasonCode = serverHangupReason;
    MessageCallback  onDisconnect;
    {
        const unique_lock lock(m_mutex);
        m_isHangup = true;
        closeSocketUnlocked(abortive);
        onDisconnect = m_onDisconnect;
    }

    if (onDisconnect)
    {
        onDisconnect(make_shared<DisconnectMessage>(reasonCode));
    }
}

bool Session::hasHangup() const
{
    const shared_lock lock(m_mutex);
    return m_isHangup;
}

void Session::onDisconnect(MessageCallback messageCallback)
{
    const unique_lock lock(m_mutex);
    m_onDisconnect = std::move(messageCallback);
}

bool Session::isConnected() const
{
    const auto state = m_state.load();
    return state == State::Connected || state == State::Connecting;
}

void Session::closeSocketUnlocked(const bool abortive)
{
    using enum State;
    // Teardown is needed from Connecting as well: a failed or aborted 'connect' may leave
    // an open socket already registered for event monitoring.
    if (transitionUnlocked(Connected, Disconnecting) ||
        transitionUnlocked(Connecting, Disconnecting))
    {
        stopKeepAlive();
        monitorIncomingDataUnlocked(false);

        if (const auto socket = getSocket())
        {
            if (abortive)
            {
                // Abrupt termination (hangup/error): close with RST and skip TIME-WAIT. Any
                // in-flight data may be discarded, which is acceptable for an abnormal close.
                static constexpr linger so_linger {.l_onoff = 1, .l_linger = 0};
                setsockopt(socket->fd(), SOL_SOCKET,
                           SO_LINGER, reinterpret_cast<const char*>(&so_linger), sizeof so_linger);
            }
            // Otherwise close in an orderly way (FIN) so a previously sent DISCONNECT is
            // reliably delivered to the server before the connection drops; an RST here would
            // discard it and make the server treat a graceful disconnect as a lost connection
            // (spuriously publishing the Last Will message).

            socket->close();
            setSocket(nullptr);
        }
    }
    setStateUnlocked(Disconnected);
}

void Session::stopKeepAlive()
{
    const lock_guard lock(m_keepAliveMutex);
    if (m_keepAliveEvent)
    {
        m_keepAliveEvent->cancel();
        m_keepAliveEvent.reset();
    }
}

MessageId Session::postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet&, bool retain)
{
    const auto messageId = nextSendMessageId();
    auto       messageDispatch = make_shared<MessageDispatch>(message, qos, messageId, message->isDuplicate(), retain);
    m_sessionThread->sendMessage(shared_from_this(), messageDispatch);

    // MQTT wants a ping only when nothing else was sent. The deadline used to move on the ping
    // alone, so a session publishing twice a second still pinged on schedule - a hundred thousand
    // of them, through one serial timer.
    if (const auto period = m_keepAlivePeriod.load(std::memory_order_relaxed);
        period > 0s)
    {
        m_nextKeepAliveTime.store(DateTime::clock::now() + period, std::memory_order_relaxed);
    }
    return messageId;
}

SendReceiveResult Session::receiveMessages()
{
    size_t byteCounter = 0;
    size_t messageCounter = 0;
    size_t publishCounter = 0;

    // Refill the per-session buffer with a single recv up front. A OneShot event precedes
    // every drain, so the socket is known readable and no readiness pre-check is needed;
    // fillReadBufferUnlocked() sizes the read from the socket's pending byte count.
    bool connectionLost = false;
    {
        const unique_lock lock(m_mutex);
        m_ackBatchBuffer.bytes(0);
        if (const auto socket = getSocket())
        {
            discardConsumedReadBufferUnlocked();
            try
            {
                fillReadBufferUnlocked(socket.get());
            }
            catch (const ConnectionException&)
            {
                connectionLost = true;
            }
            catch (const Exception& e)
            {
                m_logger->error(e.what());
                connectionLost = true;
            }
        }
    }
    if (connectionLost)
    {
        // A failed recv must not propagate: the session thread's receive loop has no
        // handler, and an escaped exception would silently kill the shared thread.
        hangup(ReasonCode::ServerUnavailable);
        return {0, 0, 0};
    }

    uint64_t packetReceivedTS = 0;
    uint64_t packetBeforeDecodeTS = 0;

    // Dispatch every complete packet now buffered (the remainder, if any, is a partial frame
    // kept for the next event). One message is dispatched per lock acquisition, and the
    // session is re-checked each iteration so dispatch stops promptly once it is torn down.
    while (true)
    {
        SMessage message;
        {
            const unique_lock lock(m_mutex);

            if (!getSocket())
            {
                break;
            }

            auto empty = true;
            auto offset = m_readConsumed;
            packetReceivedTS = LatencyTrace::now();
            auto packet = protocol().packetReader()->readPacket(m_readBuffer, empty, offset);
            if (empty)
            {
                // Only a partial packet (or nothing) remains.
                break;
            }
            m_readConsumed = offset;

            byteCounter += packet.fullSize();
            packetBeforeDecodeTS = LatencyTrace::now();
            message = protocol().messageReader()->readMessage(std::move(packet), *this, packetReceivedTS);
            if (!message)
            {
                continue;
            }

            if (message->is(Message::Type::Publish) && message->getQos() != Qos::Qos0)
            {
                // Batched under the lock already held: the acks go out in one write after
                // the drain, instead of one write syscall per received message.
                appendAckBatchUnlocked(*message);
            }
        }

        const auto messageType = message->type();
        using enum Message::Type;
        switch (messageType)
        {
            case Publish:
                ++publishCounter;
                break;
            case PublishReceived:
            case PublishRelease:
            case PublishAck:
            case PublishComplete:
            case UnsubscribeAck:
            case SubscribeAck:
                if (const auto& queue = getInflightQueueUnlocked())
                {
                    queue->receiveAck(message->getId(), messageType);
                }
                if (messageType == PublishRelease)
                {
                    ++publishCounter;
                }
                break;
            case Undefined:
            case Connect:
            case Subscribe:
            case Unsubscribe:
            case PingReq:
                break;
            case ConnectAck:
                if (const auto ackProperties = message->getProperties())
                {
                    Latency::SET_LATENCY(ackProperties, LatencyPhase::ClientWireIn, packetReceivedTS);
                    Latency::SET_LATENCY(ackProperties, LatencyPhase::ClientBeforeDecode, packetBeforeDecodeTS);
                    const auto decoded = LatencyTrace::now();
                    Latency::SET_LATENCY(ackProperties, LatencyPhase::ClientDecode, decoded);
                    Latency::SET_LATENCY(ackProperties, LatencyPhase::ClientReceive, decoded);
                }
                {
                    MessageCallback onConnectCompleted;
                    {
                        const shared_lock lock(m_mutex);
                        onConnectCompleted = m_onConnectCompleted;
                    }
                    if (onConnectCompleted)
                    {
                        onConnectCompleted(message);
                    }
                }
                if (const auto connectAckMessage = dynamic_pointer_cast<ConnectAckMessage>(message))
                {
                    if (const auto rc = connectAckMessage->getReasonCode();
                        rc != ReasonCode::Success)
                    {
                        hangup(rc);
                    }
                }
                break;
            case PingResp:
                break;
            case Disconnect:
                if (const auto disconnectMessage = dynamic_pointer_cast<DisconnectMessage>(message))
                {
                    {
                        // Acks accumulated for earlier messages of this batch must go out
                        // before hangup() closes the socket.
                        const unique_lock lock(m_mutex);
                        flushAckBatchUnlocked();
                    }
                    const auto rc = disconnectMessage->getReasonCode();
                    hangup(rc);
                }
                break;
        }

        // ConnectAck's ClientReceive is set explicitly above (it needs to share the ConnectAck's
        // own ClientDecode timestamp); every other traced message type is snapped here, right
        // before the subscriber's callback runs.
        if (messageType != ConnectAck)
        {
            if (const auto messageProperties = message->getProperties())
            {
                Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ClientReceive);
            }
        }

        executeMessageCallback(message);

        ++messageCounter;
    }

    {
        // Send the accumulated acks with a single write, and drop the consumed prefix so
        // the buffer doesn't retain processed bytes.
        const unique_lock lock(m_mutex);
        flushAckBatchUnlocked();
        discardConsumedReadBufferUnlocked();
    }

    if (!hasHangup())
    {
        monitorIncomingData(true);
    }

    return {byteCounter, messageCounter, publishCounter};
}

void Session::fillReadBufferUnlocked(sptk::TCPSocket* socket)
{
    // One syscall per pass, not two. Asking FIONREAD for the exact size and then reading it made
    // every pass that found nothing pay for the question: the broker's side of the same change
    // took 203000 read-path syscalls a second down to 131000 and 2.6% off its CPU. The client
    // does more of these than the broker does - it is the load generator's own bottleneck.
    //
    // The scratch buffer is per receive thread, not per session: sessions share threads here, and
    // a read-sized buffer on each of 100000 of them is the allocation that was measured and
    // reverted when a 256Kb per-session reserve was tried.
    constexpr size_t           maxChunkBytes = 256 * 1024;
    static thread_local Buffer scratch(maxChunkBytes);
    for (;;)
    {
        const auto received = socket->readAvailable(scratch.data(), maxChunkBytes);
        if (received == sptk::Socket::RECV_RETRY || received == 0)
        {
            return;
        }
        m_readBuffer.append(scratch.data(), received);
        // Read until the socket says RECV_RETRY, whatever kind it is. A short read used to be
        // taken as proof that a plain socket was empty, which is true at the instant it returns
        // and not a moment later: with an edge-triggered registration, bytes that arrive between
        // the read and this check are reported by nothing, and the session stalls until its next
        // message happens to wake it. TLS sockets were already read this way, hand over one
        // record at a time.
        if constexpr (clientSocketEventsMode == SocketPoolTriggerMode::OneShot)
        {
            if (!socket->readsInRecords() && received < maxChunkBytes)
            {
                return;
            }
        }
    }
}

void Session::discardConsumedReadBufferUnlocked()
{
    if (m_readConsumed == 0)
    {
        return;
    }
    if (m_readConsumed >= m_readBuffer.bytes())
    {
        m_readBuffer.bytes(0);
    }
    else
    {
        m_readBuffer.erase(0, m_readConsumed);
    }
    m_readConsumed = 0;
}

void Session::monitorIncomingData(const bool monitor)
{
    const unique_lock lock(m_mutex);
    monitorIncomingDataUnlocked(monitor);
}

void Session::monitorIncomingDataUnlocked(const bool monitor)
{
    const auto socket = getSocket();
    if (!socket || !socket->active())
    {
        return;
    }

    if (monitor)
    {
        if (m_isMonitoring)
        {
            if constexpr (clientSocketEventsMode == SocketPoolTriggerMode::OneShot)
            {
                // OneShot re-arm: the registration is unchanged, only EPOLL_CTL_MOD is issued.
                m_socketEvents.rearm(socket);
            }
            // Edge-triggered: the socket is still armed, and saying so again costs a syscall
            // under the kernel's epoll mutex for nothing.
        }
        else
        {
            m_socketEvents.add(socket, shared_from_this());
            m_isMonitoring = true;
        }
    }
    else if (m_isMonitoring)
    {
        m_socketEvents.remove(socket);
        m_isMonitoring = false;
    }
}

void Session::sendMessage(SMessageDispatch& messageDispatch)
{
    inflightQueueUnlocked()->queueMessage(messageDispatch);
}

void Session::sendMessageUnlocked(const MessageDispatch& messageDispatch)
{
    const auto* message = messageDispatch.m_message.get();

    if (message->is(Message::Type::Publish))
    {
        if (const auto properties = message->getProperties())
        {
            Latency::SNAP_LATENCY(properties, LatencyPhase::ClientWireOut);
        }
    }

    const auto& sendBuffer = writeMessageUnlocked(messageDispatch);

    if (sendBuffer.empty())
    {
        // Includes expired messages: writeMessageUnlocked() returns an empty buffer for them.
        return;
    }

    if (message->is(Message::Type::Publish))
    {
        incrementPublishSendCount();
    }

    if (const auto socket = getSocket();
        socket && socket->active())
    {
        try
        {
            socket->write(sendBuffer);
        }
        catch (const ConnectionException&)
        {
            closeSocketUnlocked();
        }
        catch (const Exception& e)
        {
            m_logger->error(e.what());
            closeSocketUnlocked();
        }
    }

    if (messageDispatch.m_message->is(Message::Type::Disconnect))
    {
        m_isDisconnectSent.post();
    }
}

#ifdef min
#undef min
#endif

tuple<DateTime::time_point, bool> Session::nextKeepAlive()
{
    // No lock: this runs on the one timer thread that serves every session in the process, and
    // taking m_mutex here put it behind the receive path of whichever session was busiest.
    const auto period = m_keepAlivePeriod.load(std::memory_order_relaxed);
    if (period <= 0s)
    {
        return {DateTime::time_point::min(), false};
    }

    const auto now = DateTime::clock::now();
    if (const auto deadline = m_nextKeepAliveTime.load(std::memory_order_relaxed);
        now < deadline)
    {
        // Something else went out since the last ping was due, which is what MQTT asks a ping to
        // stand in for. Nothing to send; come back when the traffic stops.
        return {deadline, false};
    }

    const auto deadline = now + period;
    m_nextKeepAliveTime.store(deadline, std::memory_order_relaxed);
    return {deadline, true};
}

void Session::scheduleKeepAlive(const bool init)
{
    if (auto [nextKeepAliveTime, sendKeepAlive] = nextKeepAlive();
        nextKeepAliveTime != DateTime::time_point::min())
    {
        if (sendKeepAlive && !init)
        {
            const auto ping = make_shared<AckMessage>(Message::Type::PingReq, static_cast<MessageId>(0));
            postMessage(ping, false);
        }

        auto weakSelf = weak_from_this();
        auto keepAliveEvent = keepAliveTimer.fireAt(nextKeepAliveTime,
                                                    [weakSelf]
                                                    {
                                                        if (const auto self = weakSelf.lock())
                                                        {
                                                            self->scheduleKeepAlive();
                                                        }
                                                    });

        // m_state is atomic, and the event has a lock of its own, so re-arming never waits on the
        // receive path.
        const lock_guard lock(m_keepAliveMutex);
        if (m_state == State::Disconnected)
        {
            // The session was torn down while the event was being created: don't resurrect
            // the keep-alive cycle that stopKeepAlive() has already cancelled.
            keepAliveEvent->cancel();
        }
        else
        {
            m_keepAliveEvent = std::move(keepAliveEvent);
        }
    }
}

void Session::enableKeepAlive(const bool enable)
{
    {
        if (!enable)
        {
            stopKeepAlive();
            return;
        }
        const lock_guard lock(m_keepAliveMutex);
        if (m_keepAliveEvent)
        {
            // Already scheduled.
            return;
        }
    }
    // Called without the lock: scheduleKeepAlive() acquires it internally.
    scheduleKeepAlive(true);
}

bool Session::waitForDisconnectSent(const chrono::milliseconds& timeout)
{
    return m_isDisconnectSent.wait_for(timeout);
}

void Session::queueReceiveMessages()
{
    // The receive queue stores a weak_ptr, so hand it one directly: weak_from_this()
    // skips the shared_ptr round trip (two refcount updates per event) that
    // shared_from_this() plus the queue's downgrade would cost.
    m_sessionThread->queueReceiveMessages(weak_from_this());
}

bool Session::isCleanSession() const
{
    const shared_lock lock(m_mutex);
    return m_connectProperties.m_cleanSession;
}

void Session::autoAck(const Message* message)
{
    const auto            ackMessage = make_shared<AckMessage>(messageTypeToAckType(message->type(), message->getQos()), message->getId());
    const MessageDispatch messageDispatch(ackMessage, Qos::Qos1, message->getId());

    const unique_lock lock(m_mutex);
    sendMessageUnlocked(messageDispatch);
}

void Session::appendAckBatchUnlocked(const Message& message)
{
    const auto            ackMessage = make_shared<AckMessage>(messageTypeToAckType(message.type(), message.getQos()), message.getId());
    const MessageDispatch messageDispatch(ackMessage, Qos::Qos1, message.getId());
    m_ackBatchBuffer.append(writeMessageUnlocked(messageDispatch));
}

void Session::flushAckBatchUnlocked()
{
    if (m_ackBatchBuffer.empty())
    {
        return;
    }

    if (const auto socket = getSocket();
        socket && socket->active())
    {
        try
        {
            socket->write(m_ackBatchBuffer);
        }
        catch (const ConnectionException&)
        {
            closeSocketUnlocked();
        }
        catch (const Exception& e)
        {
            m_logger->error(e.what());
            closeSocketUnlocked();
        }
    }
    m_ackBatchBuffer.bytes(0);
}

const string& Session::getClientId() const
{
    const shared_lock lock(m_mutex);
    return m_clientId;
}

string_view Session::getClientIdUnlocked() const
{
    return m_clientId;
}

string_view Session::getUsername() const
{
    const shared_lock lock(m_mutex);
    return m_username;
}

string Session::prefix() const
{
    const shared_lock lock(m_mutex);
    return m_prefix;
}

int64_t Session::recordId() const
{
    return 0;
}

SStorage Session::storage() const
{
    return nullptr;
}

bool Session::isSendQueueEmpty() const
{
    if (m_sessionThread)
    {
        return m_sessionThread->sendQueueIsEmpty();
    }
    return true;
}
