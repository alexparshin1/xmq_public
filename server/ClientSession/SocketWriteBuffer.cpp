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

#include "SocketWriteBuffer.h"
#include "ClientSession.h"

using namespace std;
using namespace sptk;
using namespace xmq;

SocketWriteBuffer::SocketWriteBuffer(ClientSession* clientSession, const MessageWriter* writer, size_t size)
    : m_writer(writer)
    , m_buffer(make_unique<Buffer>(size))
    , m_clientSession(clientSession)
{
}

bool SocketWriteBuffer::appendMessageToBuffer(const Message& message, const MessageFlags messageFlags, const MessageId& deliveryId,
                                              const uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                                              const size_t maxPacketSize)
{
    std::scoped_lock lock(m_mutex);

    const auto bufferWasEmpty = m_buffer->empty();
    const auto rc = m_writer->appendMessageToBuffer(*m_buffer, message, messageFlags, deliveryId, remainingExpirationSeconds,
                                                    subscriptionIds, maxPacketSize);
    queueMessageIfBufferWasEmpty(bufferWasEmpty);
    ++m_messageCount;
    if (message.is(Message::Type::Publish))
    {
        ++m_publishCount;
    }
    return rc;
}


void SocketWriteBuffer::appendAckToBuffer(const Message* message, const ReasonCode reasonCode)
{
    std::scoped_lock lock(m_mutex);
    const auto       bufferWasEmpty = m_buffer->empty();
    m_writer->appendAckToBuffer(*m_buffer, message, reasonCode);
    queueMessageIfBufferWasEmpty(bufferWasEmpty);
    ++m_messageCount;
}

void SocketWriteBuffer::appendDisconnectToBuffer(const ReasonCode reasonCode)
{
    std::scoped_lock lock(m_mutex);
    const auto       bufferWasEmpty = m_buffer->empty();
    m_writer->appendDisconnectToBuffer(*m_buffer, reasonCode);
    queueMessageIfBufferWasEmpty(bufferWasEmpty);
    ++m_messageCount;
}

void SocketWriteBuffer::queueMessageIfBufferWasEmpty(const bool bufferWasEmpty) const
{
    if (bufferWasEmpty && !m_buffer->empty())
    {
        m_clientSession->clientSessionSendThread()->queueProcessSession(m_clientSession->shared());
    }
}

void SocketWriteBuffer::detach(std::unique_ptr<Buffer>& exchangeBuffer)
{
    std::scoped_lock lock(m_mutex);
    m_buffer.swap(exchangeBuffer);
    m_buffer->bytes(0);
    m_messageCount = 0;
    m_publishCount = 0;
}

void SocketWriteBuffer::send(std::unique_ptr<Buffer>& exchangeBuffer)
{
    detach(exchangeBuffer);
    // The session may have disconnected (and cleared its socket) between the send thread being
    // queued and this task running; if so, there is nothing left to write to.
    if (const auto& socket = m_clientSession->getSocket())
    {
        socket->write(*exchangeBuffer);
    }
}

size_t SocketWriteBuffer::bytes() const
{
    std::scoped_lock lock(m_mutex);
    return m_buffer->bytes();
}

size_t SocketWriteBuffer::messageCount() const
{
    return m_messageCount;
}

size_t SocketWriteBuffer::publishCount() const
{
    return m_publishCount;
}
