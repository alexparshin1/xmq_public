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

#include "BaseClientSession.h"

using namespace std;
using namespace sptk;
using namespace xmq;

MessageId BaseClientSession::nextSendMessageId()
{
    // Zero is not a packet id, so the counter steps over it. Only the caller that draws the zero
    // takes the second step, because fetch_add hands every caller its own value.
    if (const auto messageId = static_cast<MessageId>(m_nextMessageId.fetch_add(1, std::memory_order_relaxed) + 1);
        messageId != 0)
    {
        return messageId;
    }
    return static_cast<MessageId>(m_nextMessageId.fetch_add(1, std::memory_order_relaxed) + 1);
}

BaseClientSession::~BaseClientSession()
{
    SMessageQueue inflightQueue;
    {
        const unique_lock lock(m_mutex);
        inflightQueue.swap(m_inflightQueue);
    }
    // The queue is destroyed here, outside m_mutex: MessageQueue's destructor takes the queue
    // mutex, while the forward-message path acquires the same two locks in the opposite order
    // (queue mutex, then session mutex).
}

const Buffer& BaseClientSession::writeMessageUnlocked(const MessageDispatch& messageDispatch)
{
    const auto& message = messageDispatch.m_message;

    m_writeBuffer.bytes(0);

    uint32_t remainingExpirationSeconds = 0;
    if (message->isExpired(remainingExpirationSeconds))
    {
        return m_writeBuffer;
    }

    protocol().messageWriter()->appendMessageToBuffer(m_writeBuffer, *message, messageDispatch.m_flags, messageDispatch.m_deliveryId,
                                                      remainingExpirationSeconds, messageDispatch.m_subscriptionIds, getMaximumPacketSize());
    return m_writeBuffer;
}

void BaseClientSession::touchLastClientMessageTimestamp()
{
    m_lastClientMessageTimestamp = DateTime::clock::now();
}

DateTime::time_point BaseClientSession::getLastClientMessageTimestamp() const
{
    return m_lastClientMessageTimestamp;
}

void BaseClientSession::clearProtocol()
{
    // Implemented in derived classes
}

void BaseClientSession::setProtocol(const GenericProtocol& protocol)
{
    m_protocol = &protocol;
}

void BaseClientSession::setInflightLimit(const uint16_t maxInflightMessages)
{
    m_maxInflightMessages.store(maxInflightMessages, std::memory_order_relaxed);
    if (m_inflightQueue)
    {
        m_inflightQueue->setMaxInflightMessages(maxInflightMessages);
    }
}

const SMessageQueue& BaseClientSession::inflightQueueUnlocked() const
{
    if (!m_inflightQueue)
    {
        // The cast says out loud what the callback has always done: the queue hands a message back
        // to the session, and the session writes it to a socket. Sending from a const method is how
        // this worked before too - the lambda was built in the constructor and captured a
        // non-const this, which const methods then used through the queue.
        auto* self = const_cast<BaseClientSession*>(this);
        m_inflightQueue = make_shared<MessageQueue>(m_maxInflightMessages.load(std::memory_order_relaxed),
                                                    [self](const SMessageDispatch& messageDispatch)
                                                    {
                                                        self->forwardMessage(messageDispatch);
                                                    });
    }
    return m_inflightQueue;
}

std::vector<SMessageDispatch> BaseClientSession::enqueuedMessages() const
{
    const auto& queue = getInflightQueueUnlocked();
    return queue ? queue->enqueuedMessages() : std::vector<SMessageDispatch>{};
}

void BaseClientSession::setMaxInflightMessages(const uint16_t maxInflightMessages) const
{
    m_maxInflightMessages.store(maxInflightMessages, std::memory_order_relaxed);
    if (const auto& queue = getInflightQueueUnlocked())
    {
        queue->setMaxInflightMessages(maxInflightMessages);
    }
}

void BaseClientSession::applyConnectProperties(const IMessageProperties& properties, ReasonCode& responseCode)
{
    if (int64_t maxPacketSize = 0;
        properties.getProperty(Property::MaximumPacketSize, maxPacketSize))
    {
        if (maxPacketSize != 0)
        {
            setMaximumPacketSize(maxPacketSize);
        }
        else
        {
            responseCode = ReasonCode::ProtocolError;
        }
    }

    if (int64_t maxInFlightMessages = 0;
        properties.getProperty(Property::ReceiveMaximum, maxInFlightMessages) && maxInFlightMessages == 0)
    {
        responseCode = ReasonCode::ProtocolError;
    }


    if (int64_t topicAliasMaximum = 0;
        properties.getProperty(Property::TopicAliasMaximum, topicAliasMaximum))
    {
        setTopicAliasMaximum(static_cast<uint16_t>(topicAliasMaximum));
    }

    if (string_view responseTopic;
        properties.getProperty(Property::ResponseTopic, responseTopic) && responseTopic.find_first_of("#%") != string::npos)
    {
        responseCode = ReasonCode::ProtocolError;
    }
}

void BaseClientSession::closeSession()
{
    if (const auto socket = getSocket();
        socket && socket->active())
    {
        socket->close();
    }
    clearProtocol();
}

void BaseClientSession::onMessage(MessageCallback messageCallback)
{
    m_messageCallback = std::move(messageCallback);
}

string BaseClientSession::bridgeOrigin() const
{
    static const string empty;
    return empty;
}

void BaseClientSession::setSocket(const STCPSocket& socket)
{
    // Release, to pair with the acquire in getSocket(): a reader that sees this pointer must
    // also see the socket it points at fully constructed.
    m_socket.store(socket, std::memory_order_release);
}