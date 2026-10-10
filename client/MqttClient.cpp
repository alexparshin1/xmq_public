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

#include "MqttClient.h"

#include "base/AckMessage.h"
#include "base/ProtocolException.h"
#include "common/DisconnectMessage.h"
#include "common/mqtt/PublishMessage.h"
#include <random>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace xmq::client;

MqttClient::MqttClient(const std::shared_ptr<LogEngine>& logEngine,
                       const std::string&                logPrefix,
                       String                            bindAddress)
    : m_logEngine(logEngine)
    , m_bindAddress(std::move(bindAddress))
{
    auto actualLogPrefix = logPrefix.empty() ? "(MqttClient) " : logPrefix;
    m_hasCallerLogPrefix = !logPrefix.empty();
    if (m_logEngine)
    {
        m_logger = make_shared<Logger>(*m_logEngine, actualLogPrefix);
    }

    static atomic_size_t nextClientIndex = 0;
    m_clientIndex = nextClientIndex++;
}

MqttClient::~MqttClient()
{
    m_messageCallbackState->closing = true;
    disconnect();
    // Receivers retain the guard independently of this object. New calls see closing;
    // calls already in previewMessage must return before the members are destroyed.
    while (m_messageCallbackState->active.load() != 0)
    {
        this_thread::sleep_for(1ms);
    }
}

ReasonCode MqttClient::connectInternal(const Host&                host,
                                       const ConnectCredentials&  credentials,
                                       const ConnectParameters&   connectParameters,
                                       ProtocolVersion            protocolVersion,
                                       const SMessageProperties&  messageProperties,
                                       const shared_ptr<SSLKeys>& sslKeys,
                                       const MessageCallback&     onConnectCompleted)
{
    if (isConnected())
    {
        disconnect();
    }

    auto logger = m_logger;
    if (logger && !m_hasCallerLogPrefix)
    {
        // The client id is the best label available by default. A caller that supplied its own
        // prefix has a better one - a bridge names the configuration entry it belongs to, which
        // the client id cannot do, since several bridges may share one.
        const auto prefixText = "(" + credentials.getClientId() + ") ";
        logger->prefix(prefixText);
    }

    // The connection in force, kept so that a reconnect can be the same connection, and the flag
    // that says whether a disconnection from here on is one somebody asked for.
    m_disconnectRequested.store(false);
    {
        const unique_lock lock(m_connectArgumentsMutex);
        m_connectArguments = make_shared<const CConnectArguments>(
            CConnectArguments {host, credentials, connectParameters, protocolVersion,
                               messageProperties, sslKeys, true});
    }

    auto session = make_shared<Session>(
        [this, callbackState = m_messageCallbackState](const SMessage& message)
        {
            ++callbackState->active;
            const struct CallbackCompleted
            {
                MessageCallbackState& state;
                ~CallbackCompleted() { --state.active; }
            } completed {*callbackState};
            if (!callbackState->closing.load())
            {
                previewMessage(message);
            }
        },
        logger, m_bindAddress);

    // The client's own handler rather than the caller's callback directly: a disconnection the
    // caller did not ask for is the moment to bring the client back, and this is the one place that
    // sees every disconnection. executeOnDisconnect reads the caller's callback as it fires, so one
    // set after this point still receives the event - which is why onDisconnect() no longer writes
    // to the session.
    session->onDisconnect([this, callbackState = m_messageCallbackState](const SMessage& message)
                          {
                              if (callbackState->closing.load())
                              {
                                  return;
                              }
                              executeOnDisconnect(message);
                              onUnexpectedDisconnect(message);
                          });

    ReasonCode result;
    try
    {
        result = session->connect(host, credentials, protocolVersion,
                                  connectParameters, messageProperties, sslKeys,
                                  onConnectCompleted);
        m_publishReceiveCount = 0;
        m_publishSentCount = 0;
    }
    catch (const Exception& exception)
    {
        if (logger)
        {
            logger->error(exception.what());
        }
        session = nullptr;
        result = ReasonCode::ErrorServerNotAvailable;
    }

    if (logger && !onConnectCompleted) // Synchronous mode.
    {
        if (result == ReasonCode::Success)
        {
            logger->debug([&host, &connectParameters]
                          {
                              const string sessionType = connectParameters.m_cleanSession ? "clean" : "persistent";
                              return "Connected " + sessionType + " session to " + host.toString() + ".";
                          });
        }
        else
        {
            logger->error(host.toString() + ": " + toString(result) + ".");
        }
    }

    m_session.store(session);

    return result;
}

void MqttClient::closeSession()
{
    if (const auto session = m_session.exchange(nullptr))
    {
        session->closeSocketUnlocked();
        if (m_logger && m_logger->has(LogPriority::Debug))
        {
            m_logger->debug("Disconnected.");
        }
    }
}

const string& MqttClient::getClientId() const
{
    static const string emptyString;
    if (const auto session = m_session.load())
    {
        return session->getClientId();
    }
    return emptyString;
}

void MqttClient::disconnect() const
{
    // Said here, where the caller is the one disconnecting: a disconnection from anywhere else is
    // one to bring the client back from, when it was asked to come back.
    m_disconnectRequested.store(true);
    if (const auto session = m_session.load(); session && session->isConnected())
    {
        const auto disconnectMessage = make_shared<DisconnectMessage>(ReasonCode::Success, Qos::Qos1);
        session->postMessage(disconnectMessage, false);
        session->waitForDisconnectSent(100ms);
        session->onMessage({});
        // Orderly (FIN) close so the DISCONNECT just sent is delivered before the socket drops;
        // an abortive RST could discard it and make the server publish the Last Will message.
        session->hangup(ReasonCode::Success, false);
        if (m_logger && m_logger->has(LogPriority::Debug))
        {
            m_logger->debug("Disconnected.");
        }
    }
}

void MqttClient::hangup() const
{
    m_disconnectRequested.store(true);
    if (const auto session = m_session.load(); session && session->isConnected())
    {
        session->hangup(ReasonCode::Success);
    }
}

MessageId MqttClient::sendMessage(const SMessage& message, const bool retain)
{
    const SSession                 session = m_session.load();
    const std::shared_ptr<Logger>& logger = m_logger;

    if (!session || !session->isConnected())
    {
        throw Exception("Not connected");
    }

    // Guard the log lambda: building the OutputString is wasted work when Debug is disabled.
    if (logger && logger->has(LogPriority::Debug))
    {
        logger->debug([&message]
                      {
                          return "Sent: " + message->toString() + ".";
                      });
    }

    const MessageId messageId = session->postMessage(message, retain);

    if (message->getQos() == Qos::Qos0 && message->is(Message::Type::Publish))
    {
        ++m_publishSentCount;
    }

    return messageId;
}

void MqttClient::executeOnPublishMessage(const SMessage& message) const
{
    // Counted for the whole load-then-invoke window, not just the invoke: onMessage({}) needs to
    // know a call that already loaded the previous (about-to-be-replaced) callback is still live,
    // even before it reaches the actual invocation below.
    ++m_activePublishCallbacks;
    const struct Decrementer
    {
        std::atomic_size_t& count;
        ~Decrementer() { --count; }
    } decrementer {m_activePublishCallbacks};

    const auto callback = m_onMessage.load();
    if (!callback || !*callback)
    {
        return;
    }

    if (const auto publishMessage = dynamic_pointer_cast<mqtt::PublishMessage>(message))
    {
        (*callback)(publishMessage);
    }
}

void MqttClient::onUnexpectedDisconnect(const SMessage& message)
{
    if (m_disconnectRequested.exchange(false))
    {
        // Somebody asked for this one, and has been answered.
        return;
    }

    std::shared_ptr<const CConnectArguments> arguments;
    {
        const unique_lock lock(m_connectArgumentsMutex);
        arguments = m_connectArguments;
    }

    if (!arguments || !arguments->m_parameters.m_autoReconnect ||
        arguments->m_parameters.m_reconnectAttempts <= 0)
    {
        return;
    }

    if (m_logger && m_logger->has(LogPriority::Debug))
    {
        const auto disconnectMessage = dynamic_pointer_cast<DisconnectMessage>(message);
        m_logger->debug(format("Disconnected ({}); reconnecting.",
                               disconnectMessage ? toString(disconnectMessage->getReasonCode())
                                                 : "unknown reason"));
    }

    startReconnect();
}

void MqttClient::startReconnect()
{
    // One attempt at a time: a worker that is already trying is doing this job, and starting another
    // on top of it - from the disconnection the first one causes, most of all - would have the two
    // race for the session.
    if (m_reconnecting.exchange(true))
    {
        return;
    }

    std::shared_ptr<const CConnectArguments> arguments;
    {
        const unique_lock lock(m_connectArgumentsMutex);
        arguments = m_connectArguments;
    }
    if (!arguments || !arguments->m_valid)
    {
        m_reconnecting.store(false);
        return;
    }

    m_reconnectWorker = std::jthread(
        [this, callbackState = m_messageCallbackState, arguments](const std::stop_token& token)
        {
            // The waiting grows and is scattered; see ConnectParameters. The scatter is drawn per
            // client, from its own index, so that two clients of a run never share a moment - which
            // is what keeps a returning node from being reached by every one of them at once.
            mt19937_64 random(static_cast<uint64_t>(m_clientIndex) + 1);
            uniform_real_distribution<double> scatter(1.0 - arguments->m_parameters.m_reconnectJitter,
                                                      1.0 + arguments->m_parameters.m_reconnectJitter);
            const auto maxInterval = chrono::milliseconds(arguments->m_parameters.m_reconnectMaxInterval);
            auto       interval = chrono::milliseconds(arguments->m_parameters.m_reconnectInterval);

            for (int attempt = 0; attempt < arguments->m_parameters.m_reconnectAttempts; ++attempt)
            {
                const auto wait = chrono::milliseconds(static_cast<int64_t>(
                    static_cast<double>(interval.count()) * scatter(random)));

                // Waiting in small steps: an interval is a lifetime of its own, and a client being
                // destroyed should not be held up by one - the flag that guards the callbacks ends
                // the waiting too.
                for (int64_t waited = 0; waited < wait.count(); waited += 200)
                {
                    if (token.stop_requested() || callbackState->closing.load())
                    {
                        m_reconnecting.store(false);
                        return;
                    }
                    this_thread::sleep_for(200ms);
                }

                const auto host = arguments->m_parameters.m_reconnectDestination
                                      ? arguments->m_parameters.m_reconnectDestination()
                                      : arguments->m_host;
                try
                {
                    if (connectInternal(host, arguments->m_credentials, arguments->m_parameters,
                                        arguments->m_protocolVersion, arguments->m_messageProperties,
                                        arguments->m_sslKeys, nullptr) == ReasonCode::Success)
                    {
                        m_reconnecting.store(false);
                        const auto callback = m_onReconnect.load();
                        if (callback && *callback && !callbackState->closing.load())
                        {
                            (*callback)();
                        }
                        return;
                    }
                }
                catch (const std::exception&)
                {
                    // An attempt that failed is not the end of it: the next one follows after the
                    // interval. What the caller learns is a reconnect, or the absence of one.
                }

                // And the next interval is longer than this one, up to the ceiling: a server that is
                // down for a while should not be asked every two seconds by every client there is.
                const auto grown = static_cast<double>(interval.count()) * arguments->m_parameters.m_reconnectBackoff;
                interval = chrono::milliseconds(static_cast<int64_t>(
                    min(grown, static_cast<double>(maxInterval.count()))));
            }
            m_reconnecting.store(false);
        });
}

void MqttClient::executeOnDisconnect(const SMessage& message) const
{
    const auto callback = m_onDisconnect.load();
    if (!callback || !*callback)
    {
        return;
    }
    const auto disconnectMessage = dynamic_pointer_cast<DisconnectMessage>(message);
    (*callback)(make_shared<DisconnectMessage>(disconnectMessage->getReasonCode()));
}

void MqttClient::executeOnAck(const SMessage& message) const
{
    const auto callback = m_onAck.load();
    if (!callback || !*callback)
    {
        return;
    }
    (*callback)(message);
}

uint16_t MqttClient::inflightLimit() const
{
    const auto session = m_session.load();
    return session ? session->inflightLimit() : 0;
}

SSession MqttClient::getSession() const
{
    return m_session.load();
}

shared_ptr<Logger> MqttClient::getLogger() const
{
    return m_logger;
}

void MqttClient::previewMessage(const SMessage& message)
{
    if (m_logger && m_logger->has(LogPriority::Debug))
    {
        m_logger->debug([&message]
                        {
                            return format("Received: {}.", message->toString());
                        });
    }

    using enum Message::Type;
    switch (message->type())
    {
        case Publish:
            ++m_publishReceiveCount;
            executeOnPublishMessage(message);
            break;
        case Disconnect:
            break;
        case PublishComplete:
        case PublishAck:
            ++m_publishSentCount;
            executeOnAck(message);
            break;
        case PublishReceived:
        case PublishRelease:
        case ConnectAck:
        case UnsubscribeAck:
        case SubscribeAck:
        case PingResp:
            executeOnAck(message);
            break;
        case Undefined: {
            const auto session = m_session.load();
            throw ProtocolException(session->m_protocolVersion,
                                    ReasonCode::MalformedPacket,
                                    "Undefined message type");
        }
        case Connect:
        case Subscribe:
        case Unsubscribe:
        case PingReq:
            break;
    }
}

bool MqttClient::isConnected() const
{
    const auto session = m_session.load();
    return session && session->isConnected();
}

void MqttClient::onMessage(PublishMessageCallback messageCallback)
{
    const bool clearing = !messageCallback;
    m_onMessage.store(std::make_shared<const PublishMessageCallback>(std::move(messageCallback)));

    if (clearing)
    {
        // Callers clear the callback right before destroying state it captures (e.g. the
        // scenario engine's per-client latency vector). Wait for any call that already loaded
        // the previous callback to finish, so it can't run against that state after this
        // returns. Bounded so a stuck user callback can't hang shutdown indefinitely.
        constexpr auto drainTimeout = 5s;
        const auto drainDeadline = chrono::steady_clock::now() + drainTimeout;
        while (m_activePublishCallbacks.load() > 0 && chrono::steady_clock::now() < drainDeadline)
        {
            this_thread::sleep_for(1ms);
        }
    }
}

void MqttClient::onAck(MessageCallback messageCallback)
{
    m_onAck.store(std::make_shared<const MessageCallback>(std::move(messageCallback)));
}

void MqttClient::onDisconnect(MessageCallback messageCallback)
{
    // Stored, and not pushed into the session: the session's handler is the client's own, which
    // reads this callback as it fires - so one set after the connection works, and a client that
    // reconnects keeps it without being told.
    m_onDisconnect.store(std::make_shared<const MessageCallback>(std::move(messageCallback)));
}

void MqttClient::onReconnect(std::function<void()> reconnectCallback)
{
    m_onReconnect.store(std::make_shared<const std::function<void()>>(std::move(reconnectCallback)));
}

void MqttClient::subscribe(const std::string_view destination, const SMessageProperties& properties)
{
    if (const auto* topic = getTopic(destination))
    {
        const Destinations destinations {Destination(topic)};
        subscribe(destinations, properties);
    }
    else
    {
        throw Exception("Invalid destination");
    }
}

void MqttClient::subscribe(const Destination& destination, const SMessageProperties& properties)
{
    const Destinations destinations {destination};
    subscribe(destinations, properties);
}

MessageId MqttClient::subscribe(const Destinations& destinations, const SMessageProperties& properties)
{
    const auto subscribeMessage = make_shared<SubscribeMessage>(destinations);

    if (m_logger && m_logger->has(LogPriority::Debug))
    {
        Strings topics;
        for (const auto& destination: destinations)
        {
            topics.push_back(destination.m_topic->fullName().data());
        }
        m_logger->debug("Subscribe: [" + topics.join(", ") + "].");
    }

    if (properties)
    {
        subscribeMessage->setProperties(properties);
    }

    return sendMessage(subscribeMessage, false);
}

MessageId MqttClient::unsubscribe(const Destination& destination)
{
    const Destinations destinations {destination};
    return unsubscribe(destinations);
}

MessageId MqttClient::unsubscribe(const Destinations& destinations)
{
    const auto unsubscribeMessage = make_shared<UnsubscribeMessage>(destinations);
    unsubscribeMessage->setQos(Qos::Qos1);
    return sendMessage(unsubscribeMessage, false);
}

MessageId MqttClient::publishPayload(const Topic* destination, const string_view payload, const Qos qos,
                                     const SMessageProperties& properties, const bool retain, const bool duplicate)
{
    const auto publishMessage = make_shared<mqtt::PublishMessage>(destination, payload, static_cast<MessageId>(0), retain);
    if (properties)
    {
        // Skip the virtual call + shared_ptr store when there are no properties (the common case).
        publishMessage->setProperties(properties);
    }
    publishMessage->setQos(qos);
    publishMessage->setDup(duplicate);
    return sendMessage(publishMessage, retain);
}

MessageId MqttClient::publish(const Topic* destination, const Buffer& payload, const Qos qos,
                              const SMessageProperties& properties, const bool retain, const bool duplicate)
{
    return publishPayload(destination, string_view(payload.c_str(), payload.bytes()), qos, properties, retain, duplicate);
}

MessageId MqttClient::publish(const string& destination, const string& payload, const Qos qos, const bool retain)
{
    return publishPayload(getTopic(destination), payload, qos, {}, retain, false);
}

MessageId MqttClient::publish(const PublishMessage& publishMessage)
{
    // Pass the payload straight through as a string_view - no intermediate Buffer copy.
    return publishPayload(publishMessage.destination(), publishMessage.payload(),
                          publishMessage.getQos(), publishMessage.getProperties(),
                          publishMessage.isRetain(), publishMessage.isDuplicate());
}

MessageId MqttClient::ping()
{
    const auto pingMessage = make_shared<AckMessage>(Message::Type::PingReq);
    pingMessage->setQos(Qos::Qos1);
    return sendMessage(pingMessage, false);
}

void MqttClient::enableKeepAlive(const bool enable) const
{
    if (const auto session = m_session.load(); session && session->isConnected())
    {
        session->enableKeepAlive(enable);
    }
}

void MqttClient::flush(chrono::milliseconds timeout) const
{
    if (!isConnected())
    {
        return;
    }

    // Wait for all messages to be sent
    const auto flushStarted = chrono::steady_clock::now();
    while (chrono::steady_clock::now() < flushStarted + timeout)
    {
        if (const auto session = m_session.load(); session && session->isSendQueueEmpty())
        {
            break;
        }
        this_thread::sleep_for(10ms);
    }
}

size_t MqttClient::getClientIndex() const
{
    return m_clientIndex.load();
}

void MqttClient::setClientIndex(const size_t clientIndex)
{
    m_clientIndex.store(clientIndex);
}

ProtocolVersion MqttClient::protocolVersion() const
{
    if (const auto session = m_session.load())
    {
        return session->getProtocolVersion();
    }
    return ProtocolVersion::MqttV5;
}

const Topic* MqttClient::getTopic(const std::string_view topic)
{
    return Session::getTopicManager()->getTopic(topic);
}
