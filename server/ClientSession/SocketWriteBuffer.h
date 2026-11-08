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

#include "common/MessageWriter.h"

#include <atomic>

namespace xmq {

namespace sendpathdiagnostics {
// TEMPORARY 2026-11-08: where a queued send waits from the queue to its write. Exposed for the
// metrics scan only; reads and clears the counters it holds.
void noteSendWait(uint64_t elapsedNs);
void reportSendWait();
}

class BaseClientSession;
class ClientSessionThread;

/**
 * @brief Facilitates writing data to a socket utilizing a buffering mechanism.
 *
 * Optimizes socket write operations by using a buffer to store and manage outgoing data temporarily.
 * Ensures efficient network communication by minimizing the number of write system calls made to the socket.
 */
/**
 * @brief What a flush of a session's write buffer carried.
 *
 * What the send workers and the inline send path each report to the system statistics, and
 * what a shared flush across both of them came out as - the counters after a detach sit on
 * the buffer, and the flush reads them while it is the one holding it.
 */
struct SocketWriteMetrics
{
    size_t m_bytes {0};     ///< Bytes the flush put on the wire; zeros when nothing was waiting.
    size_t m_messages {0};  ///< Messages the flush emptied the buffer of.
    size_t m_publishes {0}; ///< Publishes among them.
};

class SocketWriteBuffer final
{
public:
    /**
     * @brief Constructor.
     */
    SocketWriteBuffer(ClientSession* clientSession, const MessageWriter* writer, size_t size);

    /**
     * @brief Write the message into the buffer.
     * @param message           Message.
     * @param messageFlags      Message delivery flags.
     * @param deliveryId        Message id.
     * @param remainingExpirationSeconds Remaining expiration seconds.
     * @param subscriptionIds   Matched subscriptions ids.
     * @param maxPacketSize     Maximum packet size.
     * @param notifySendWorker  Wake the send worker when the first bytes fill an empty buffer. The
     *                          inline send path puts them on the wire itself instead, so it asks
     *                          for no wake.
     */
    bool appendMessageToBuffer(const Message&           message,
                               MessageFlags             messageFlags,
                               const MessageId&         deliveryId,
                               uint32_t                 remainingExpirationSeconds,
                               const SubscriptionIdSet& subscriptionIds,
                               size_t                   maxPacketSize,
                               const bool               notifySendWorker = true);

    /**
     * @brief Send ack.
     * @param message           Message to which ack responds.
     * @param reasonCode        Ack reason code.
     */
    void appendAckToBuffer(const Message* message, ReasonCode reasonCode);

    /**
     * @brief Write disconnect message to buffer.
     * @param reasonCode        Disconnect reason code.
     */
    void appendDisconnectToBuffer(ReasonCode reasonCode);

    /**
     * @brief Take what is queued, put it on the socket, and return what went out.
     *
     * Holding the session's send mutex across taking the bytes and putting them on the wire is
     * the point of the call: the send workers and the inline send path both hand here, so one 
     * session has one writer at a time, and however many messages were appended queue in the
     * order they were appended - no path can slip between a detached buffer and its write.
     *
     * Nothing is written when nothing was queued; the exchange buffer is left sized for the
     * next round.
     *
     * @param exchangeBuffer    An empty buffer; comes back holding what was sent.
     * @return Bytes, messages and publishes the flush carried; zeros when nothing was waiting.
     */
    SocketWriteMetrics send(std::unique_ptr<sptk::Buffer>& exchangeBuffer);

    /**
     * @brief Hands the accumulated bytes over and leaves the session ready to take more.
     *
     * The swap half of send(), split out so a batch can be collected first and written after.
     * Delivery threads keep appending to the fresh buffer while the detached one is on its way to
     * the socket - which is the whole reason the buffer is exchanged rather than written in place.
     *
     * @param exchangeBuffer An empty buffer to leave behind; comes back holding what was queued.
     */
    void detach(std::unique_ptr<sptk::Buffer>& exchangeBuffer);

private:
    // TEMPORARY 2026-11-08: when this session's empty buffer was filled and queued to a send
    // worker; send() turns it into the queued wait and clears it.
    std::atomic<uint64_t>         m_enqueuedNs {0};
    mutable std::mutex            m_mutex;
    const MessageWriter*          m_writer;
    std::unique_ptr<sptk::Buffer> m_buffer;
    ClientSession*                m_clientSession;
    std::atomic_size_t            m_messageCount {0};
    std::atomic_size_t            m_publishCount {0};

    /**
     * @brief The one mutex a flush holds across taking the bytes and putting them on the wire.
     *
     * The send workers and the inline send path both write through send(), so the two shapes
     * never interleave on the same session: an empty buffer stays empty until the writer that
     * detached it is on the wire, and the bytes that did arrive leave in append order.
     */
    mutable std::mutex            m_sendMutex;

    void queueMessageIfBufferWasEmpty(bool bufferWasEmpty, bool notifySendWorker = true) const;
};

using USocketWriteBuffer = std::unique_ptr<SocketWriteBuffer>;

/**
 * @brief Shared owner of a session's write buffer.
 *
 * Shared rather than unique because setProtocol() replaces the buffer when a reconnect takes the
 * session over, while another thread may be inside appendAckToBuffer() holding the buffer's own
 * mutex. Destroying it there wrote into freed memory - unlocking a destroyed mutex - and corrupted
 * the heap. Callers take a copy for the duration of their use, so a replacement frees nothing that
 * is still in use.
 */
using SSocketWriteBuffer = std::shared_ptr<SocketWriteBuffer>;

} // namespace xmq
