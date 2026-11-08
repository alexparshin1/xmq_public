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
using namespace std::chrono;
using namespace sptk;
using namespace xmq;

namespace xmq {
namespace sendpathdiagnostics {

// TEMPORARY 2026-11-08: five buckets of the wait from a queued buffer to its write, in
// microseconds. The metrics scan reads and clears them once a second; see the header note on
// what the numbers belong to.
std::atomic<uint64_t> g_sendWaitCounts[] = {0, 0, 0, 0, 0};
// TEMPORARY 2026-11-08: the notice itself, and the flush read that cleared a stamp.
std::atomic<uint64_t> g_noticeQueued {0};
std::atomic<uint64_t> g_flushCleared {0};

void noteSendWait(const uint64_t elapsedNs)
{
    const auto us = elapsedNs / 1000;
    const size_t bucket = us < 10 ? 0 : us < 50 ? 1 : us < 200 ? 2 : us < 1000 ? 3 : 4;
    g_sendWaitCounts[bucket].fetch_add(1, std::memory_order_relaxed);
}

void reportSendWait()
{
    const auto w0 = g_sendWaitCounts[0].exchange(0, std::memory_order_relaxed);
    const auto w1 = g_sendWaitCounts[1].exchange(0, std::memory_order_relaxed);
    const auto w2 = g_sendWaitCounts[2].exchange(0, std::memory_order_relaxed);
    const auto w3 = g_sendWaitCounts[3].exchange(0, std::memory_order_relaxed);
    const auto w4 = g_sendWaitCounts[4].exchange(0, std::memory_order_relaxed);
    const auto noticed = g_noticeQueued.exchange(0, std::memory_order_relaxed);
    const auto cleared = g_flushCleared.exchange(0, std::memory_order_relaxed);
    COUT(std::format("SEND-PATH-DIAG per second: to10us={} to50={} to200={} to1ms={} over1ms={} notices={} flushWrites={}",
                     w0, w1, w2, w3, w4, noticed, cleared));
}

} // namespace sendpathdiagnostics
} // namespace xmq

SocketWriteBuffer::SocketWriteBuffer(ClientSession* clientSession, const MessageWriter* writer, size_t size)
    : m_writer(writer)
    , m_buffer(make_unique<Buffer>(size))
    , m_clientSession(clientSession)
{
}

bool SocketWriteBuffer::appendMessageToBuffer(const Message& message, const MessageFlags messageFlags, const MessageId& deliveryId,
                                              const uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                                              const size_t maxPacketSize, const bool notifySendWorker)
{
    std::scoped_lock lock(m_mutex);

    const auto bufferWasEmpty = m_buffer->empty();
    const auto rc = m_writer->appendMessageToBuffer(*m_buffer, message, messageFlags, deliveryId, remainingExpirationSeconds,
                                                    subscriptionIds, maxPacketSize);
    queueMessageIfBufferWasEmpty(bufferWasEmpty, notifySendWorker);
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

void SocketWriteBuffer::queueMessageIfBufferWasEmpty(const bool bufferWasEmpty, const bool notifySendWorker) const
{
    if (bufferWasEmpty && notifySendWorker && !m_buffer->empty())
    {
        // TEMPORARY 2026-11-08, see the header note.
        sendpathdiagnostics::g_noticeQueued.fetch_add(1, std::memory_order_relaxed);
        const_cast<std::atomic<uint64_t>&>(m_enqueuedNs).store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count());
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

SocketWriteMetrics SocketWriteBuffer::send(std::unique_ptr<sptk::Buffer>& exchangeBuffer)
{
    // One section for the taking and the sending, so no writer of this session's socket can slip
    // between a detached buffer and its write - see the comment in the header. The buffer's own
    // lock keeps the swap and the counters atomic inside it.
    std::scoped_lock sendLock(m_sendMutex);

    const auto bytes = m_buffer->bytes();
    const auto messages = m_messageCount.load(std::memory_order_relaxed);
    const auto publishes = m_publishCount.load(std::memory_order_relaxed);

    // TEMPORARY 2026-11-08, see the header's note: how long the queue carried this buffer.
    if (const auto enqueued = m_enqueuedNs.exchange(0); enqueued != 0)
    {
        sendpathdiagnostics::g_flushCleared.fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        sendpathdiagnostics::noteSendWait(now > enqueued ? now - enqueued : 0);
    }

    detach(exchangeBuffer);

    // The session may have disconnected (and cleared its socket) between the flush being queued
    // and this task running; if so, there is nothing left to write to.
    if (!bytes)
    {
        return {};
    }

    if (const auto& socket = m_clientSession->getSocket())
    {
        socket->write(*exchangeBuffer);
    }

    return {bytes, messages, publishes};
}
