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

private:
    using MessageDispatchMap = XMQ_MAP_TYPE<MessageId, SMessageDispatch>;

    using WaitingMessages = std::deque<SMessageDispatch>;

    mutable std::mutex               m_mutex;
    std::atomic_uint16_t             m_maxInflightMessages;
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
    MessageId queueMessageUnlocked(const SMessageDispatch& messageDispatch);
    MessageId restoreMessageUnlocked(SMessageDispatch& messageDispatch);
};

using SMessageQueue = std::shared_ptr<MessageQueue>;

} // namespace xmq
