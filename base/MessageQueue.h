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

#include "MessageDispatch.h"
#include "base/Message.h"

namespace xmq {

/**
 * @brief Message queue.
 * A class responsible for managing a queue of dispatched messages, supporting message
 * acknowledgment handling, message transmission, and rescheduling, as well as maintaining
 * constraints on the maximum number of in-flight messages.
 */
class MessageQueue
{
public:
    using ForwardMessage = std::function<void(const SMessageDispatch&)>;

    /**
     * @brief Constructs a MessageQueue instance.
     *
     * @param maxInflightMessages The maximum number of QOS 1 or 2 messages that can be in-flight at any given time.
     * @param forwardMessage A callable function or functor responsible for forwarding dispatched messages.
     */
    MessageQueue(uint16_t maxInflightMessages, ForwardMessage forwardMessage);

    /**
     * @brief Destructor.
     */
    ~MessageQueue();

    void      setMaxInflightMessages(uint16_t maxInflightMessages);

    /**
     * @brief Bounds on what a session may hold for a client that does not keep up.
     *
     * The in-flight window is the client's own (its CONNECT receive maximum, capped by the
     * server); these are the bounds on everything the session holds for that client, in flight and
     * waiting alike, and they had no bound at all: a slow subscriber took the node's memory with
     * it, one session at a time, until the process died. Measured on a two-node Windows cluster:
     * 3000 subscriptions fanned out to 16 GB of virtual memory on an 8 GB machine, and both
     * brokers stopped.
     *
     * The two are independent: a session may hold at most maxMessages messages and at most
     * maxBytes bytes, and whichever it reaches first stops it. Neither is turned into the other -
     * a message count says nothing about the size of the messages, which is why both exist.
     *
     * What is already queued when a limit is lowered stays; the limits decide what is accepted,
     * not what is thrown away.
     *
     * @param maxMessages Messages the session may hold. Zero means no limit.
     * @param maxBytes    Bytes of payload and topic the session may hold. Zero means no limit.
     */
    void setQueueLimits(uint32_t maxMessages, uint64_t maxBytes);

    /**
     * @brief Whether one more message of this size can be taken.
     *
     * Asked before the delivery record is made, so that a message dropped here does not leave a
     * record behind in storage. Counts what is in flight as well as what waits: the limits bound
     * the session, and a message in the window is held by this session exactly as one behind it is.
     */
    [[nodiscard]] bool canAccept(uint64_t messageBytes) const;

    /// Messages dropped by the queue limits since the session started.
    [[nodiscard]] uint64_t droppedMessages() const;

    /**
     * @brief Allocate the next packet id in this session's id space.
     *
     * The queue owns the id space, but the id has to be known before the delivery record is
     * persisted - a record stored under a different id than the one sent to the client could not
     * be matched to an acknowledgement that arrives after the session moved to another node.
     * A delivery that already carries an id keeps it when queued.
     *
     * @return The allocated id, never 0.
     */
    MessageId nextDeliveryId();

    MessageId queueMessage(const SMessageDispatch& messageDispatch);
    MessageId restoreMessage(SMessageDispatch& messageDispatch);
    void      receiveAck(MessageId messageId, Message::Type ackType);
    void      sendWaitingMessages(const ForwardMessage& forwardMessage);
    void      rescheduleMessagesWaitingForAck();
    void      clear();
    size_t    size() const;

    /**
     * @brief Drop messages that have outlived their expiry interval.
     *
     * Expiry is otherwise only noticed on the way out to a socket, which never happens for a
     * session with no client attached: its messages would sit in the queue - and, for a
     * persistent session, in storage - for as long as the session exists.
     *
     * Messages waiting to be sent are always dropped. Whether the in-flight ones are too is the
     * caller's decision: for a session with a client attached they have genuinely been sent and
     * are awaiting acknowledgement, and discarding them would lose the acknowledgement tracking.
     * For a session with no client they have not been sent at all - queueMessage() "forwards"
     * them to a send that does nothing - so they are in-flight in name only, and are exactly the
     * messages that need dropping.
     *
     * @param secondsUntilNextExpiration Receives the seconds until the earliest expiry among the
     *                                   messages left in the queue, or 0 when none of them expire.
     * @param includeUnacknowledged      Also drop expired messages that are in flight.
     * @return the number of messages dropped.
     */
    size_t removeExpiredMessages(uint32_t& secondsUntilNextExpiration, bool includeUnacknowledged);

    std::vector<SMessageDispatch> enqueuedMessages() const;

    /**
     * @brief Call an action for every message in the queue - the queued objects themselves, where
     *        enqueuedMessages() hands out copies - in flight first, then waiting. Under the queue's lock.
     * @param action            Action to call.
     */
    void forEach(const ForwardMessage& action) const;

private:
    using MessageDispatchMap = XMQ_MAP_TYPE<MessageId, SMessageDispatch>;

    using WaitingMessages = std::deque<SMessageDispatch>;

    mutable std::mutex               m_mutex;
    std::atomic_uint16_t             m_maxInflightMessages;
    std::atomic_uint32_t             m_maxQueuedMessages {0};
    std::atomic_uint64_t             m_maxQueuedBytes {0};
    uint64_t                         m_queuedBytes {0};         ///< Sum of what this queue holds, in flight and waiting, under m_mutex.
    std::atomic_uint64_t             m_droppedMessages {0};
    MessageId                        m_nextMessageId {0};
    MessageDispatchMap               m_inflightMessages;
    std::unique_ptr<WaitingMessages> m_waitingMessages; ///< Created on first use, see waitingMessages().
    const ForwardMessage             m_forwardMessage;

    /**
     * @brief The messages waiting for a free in-flight slot, created on first use.
     *
     * Every session holds a queue, and libstdc++'s std::deque allocates its map and a first block
     * - 576 bytes - the moment it is constructed, empty or not. Most sessions never fill their
     * in-flight window, so that was 576 bytes of every idle connection's heap, measured with
     * massif. The deque is released again by clear(), not whenever it drains: a window that keeps
     * filling would otherwise allocate and free it on every acknowledgement.
     */
    WaitingMessages& waitingMessages();

    /// The number of waiting messages, without creating the queue.
    size_t waitingCount() const;

    MessageId nextDeliveryIdUnlocked();

    /// Whether accepting this message would take the session past either limit. Under m_mutex.
    [[nodiscard]] bool exceedsLimitsUnlocked(uint64_t messageBytes) const;

    /**
     * @param applyLimits Refuse the message when it would exceed a limit. False when re-queuing
     *                    what the queue already held - a reconnect re-sends messages that were
     *                    accepted under the limits as they stood then, and dropping them here
     *                    would lose messages the client was already promised.
     */
    MessageId queueMessageUnlocked(const SMessageDispatch& messageDispatch, bool applyLimits = true);
    MessageId restoreMessageUnlocked(SMessageDispatch& messageDispatch);
};

using SMessageQueue = std::shared_ptr<MessageQueue>;

} // namespace xmq
