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

namespace xmq {

class BaseClientSession;
class ClientSessionThread;

/**
 * @brief Facilitates writing data to a socket utilizing a buffering mechanism.
 *
 * Optimizes socket write operations by using a buffer to store and manage outgoing data temporarily.
 * Ensures efficient network communication by minimizing the number of write system calls made to the socket.
 */
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
     */
    bool appendMessageToBuffer(const Message&           message,
                               MessageFlags             messageFlags,
                               const MessageId&         deliveryId,
                               uint32_t                 remainingExpirationSeconds,
                               const SubscriptionIdSet& subscriptionIds,
                               size_t                   maxPacketSize);

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

    size_t bytes() const;
    size_t messageCount() const;
    size_t publishCount() const;
    void   send(std::unique_ptr<sptk::Buffer>& exchangeBuffer);

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
    mutable std::mutex            m_mutex;
    const MessageWriter*          m_writer;
    std::unique_ptr<sptk::Buffer> m_buffer;
    ClientSession*                m_clientSession;
    std::atomic_size_t            m_messageCount {0};
    std::atomic_size_t            m_publishCount {0};

    void queueMessageIfBufferWasEmpty(bool bufferWasEmpty) const;
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
