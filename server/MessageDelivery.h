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

#include "base/MessageDispatch.h"
#include "common/MessageWriter.h"
#include "storage/PersistentObject.h"

#include <sptk5/threads/TimerEvent.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace xmq {

class ClientSession;
class Server;

/**
 * @brief Message delivery.
 * Combines the message writer, message, QoS, and delivery id.
 * The delivery id is the next message id generated for the client connection.
 */
class MessageDelivery final
    : public MessageDispatch
    , public PersistentObject
{
public:
    std::shared_ptr<ClientSession> m_clientSession; ///< Client connection

    /**
     * @brief Factory.
     * @param clientSession     Client session.
     * @param message           Message.
     * @param qos               QoS.
     * @param deliveryId        Delivery id.
     * @param subscriptionIds   Matched subscription ids.
     * @param setRetainFlag     Set retain flag.
     * @param messageDeliveryRecordId   Message delivery record ID.
     */
    static std::shared_ptr<MessageDelivery> create(const std::shared_ptr<ClientSession>& clientSession, const SMessage& message, Qos qos,
                                                   const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, bool setRetainFlag,
                                                   int64_t messageDeliveryRecordId = 0);

    /**
     * @brief Factory.
     * @param clientSession     Client session.
     * @param message           Message.
     * @param qos               QoS.
     * @param deliveryId        Delivery id.
     * @param subscriptionIds   Matched subscription ids.
     * @param setRetainFlag     Set retain flag.
     * @param messageDeliveryRecordId   Message delivery record ID.
     * @param completionCallback Completion callback.
     */
    static std::shared_ptr<MessageDelivery> create(const std::shared_ptr<ClientSession>& clientSession, const SMessage& message, Qos qos,
                                                   const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, bool setRetainFlag,
                                                   int64_t messageDeliveryRecordId, const std::function<void(const std::shared_ptr<MessageDelivery>&)>& completionCallback);

    /**
     * @brief Constructor.
     * @param clientSession     Client session.
     * @param packedData        Packed data.
     */
    explicit MessageDelivery(const std::shared_ptr<ClientSession>& clientSession, const sptk::Buffer& packedData);

    /**
     * @brief Copy constructor (deleted).
     */
    MessageDelivery(const MessageDelivery&) = delete;

    /**
     * @brief Move constructor.
     */
    MessageDelivery(MessageDelivery&&) = delete;

    /**
     * @brief Move assignment
     */
    MessageDelivery& operator=(MessageDelivery&&) = delete;

    /**
     * @brief Copy assignment (deleted)
     */
    MessageDelivery& operator=(const MessageDelivery&) = delete;

    /**
     * @brief Destructor
     */
    ~MessageDelivery() override;

    /**
     * @brief Get message
     * @return Message
     */
    [[nodiscard]] SMessage message() const
    {
        return m_message;
    }

    /**
     * @brief Get delivery id
     * @return Delivery id
     */
    [[nodiscard]] MessageId deliveryId() const
    {
        return m_deliveryId;
    }

    // Persistence methods
    void pack(sptk::Buffer& record) override;
    void unpack(const sptk::Buffer& record) override;
    void storeRecordAsync(const std::function<void()>& callback) override;
    void removeRecordAsync(const std::function<void(const size_t&)>& callback) override;

    RecordId getRecordId() const
    {
        return m_recordId;
    }

    /**
     * @brief Get the number of queued operations not yet completed in Redis.
     * @return Number of queued operations.
     */
    static size_t queuedOperations()
    {
        return m_queuedOperations.load();
    }

    /**
     * @brief Wait until Redis has caught up enough to accept more record writes.
     *
     * Only used when persistence.max_queued_writes is above 0. Deliveries then run ahead of
     * their record writes instead of waiting for each one, which is what lets the writes
     * pipeline - a single Redis connection answers ~38,000 unpipelined writes/s but ~600,000
     * pipelined. This caps how far ahead they may run: at the limit a delivery blocks until the
     * backlog has halved, so the broker throttles instead of queueing without bound.
     *
     * Counts record *writes* only. Record removals are fire-and-forget and carry no durability
     * meaning, so including them would couple the throttle to unrelated work and inflate the
     * backlog - with a small window the counter then never falls to the release threshold and
     * every blocked delivery sleeps out its timeout instead.
     *
     * @param maxQueuedWrites   Outstanding writes at which to block. Must be above 0.
     */
    static void awaitWriteCapacity(size_t maxQueuedWrites);

    /**
     * @brief Would a PUBLISH received now have to wait for Redis to catch up?
     *
     * The receive path asks this before handling a QoS 1 or 2 PUBLISH, and pauses the session
     * instead of letting awaitWriteCapacity() park the receive thread.
     */
    [[nodiscard]] static bool writeCapacityExhausted(const std::shared_ptr<ClientSession>& clientSession);

    /**
     * @brief Stop reading the session until the record backlog has halved.
     *
     * Resumed from the Redis completion that brings the backlog down. WriteCapacityTimeout
     * resumes it too, as a guard against a missed wakeup: a session that still finds the backlog
     * full pauses again, so max_queued_writes stays a hard bound, and while Redis confirms nothing
     * its publishers are held by TCP instead of being written past the limit. The keep-alive check
     * skips a paused session - the silence is the broker's, not the client's.
     */
    static void pauseUntilWriteCapacity(const std::shared_ptr<ClientSession>& clientSession);

    /// How many times a session has been paused for write capacity since the process started.
    [[nodiscard]] static size_t writeCapacityPauses()
    {
        return m_writeCapacityPauses.load(std::memory_order_relaxed);
    }

private:
    static void releaseWriteCapacity();
    static void resumePausedSessions();

    static std::mutex                                m_pausedSessionsMutex;
    static std::vector<std::weak_ptr<ClientSession>> m_pausedSessions;     ///< Sessions not read until Redis catches up.
    static std::atomic_size_t                        m_pausedSessionCount; ///< m_pausedSessions.size(), read without the lock.
    static std::atomic_size_t                        m_writeCapacityPauses; ///< See writeCapacityPauses().
    static sptk::STimerEvent                         m_pauseTimeout;       ///< Resumes every paused session if Redis does not.

    static std::mutex              m_writeCapacityMutex;
    static std::condition_variable m_writeCapacityAvailable;
    static std::atomic_size_t      m_queuedWrites;          ///< Record writes issued but not yet confirmed by Redis.
    static std::atomic_size_t      m_writeCapacityWaiters;  ///< Deliveries currently blocked on Redis catching up.
    static std::atomic_size_t      m_writeCapacityResumeAt; ///< Backlog at which blocked deliveries are released.

    std::string                  m_sessionMessagesKey; ///< Redis hash key of the session's messages, captured at construction.
    bool                         m_persisted {false};  ///< True if the record exists in Redis under m_recordId; only then is it removed on destruction.
    RecordId                     m_recordId {0};
    static std::atomic<RecordId> m_recordIdSerial;
    static std::atomic_size_t    m_queuedOperations; ///< Number of queued operations not yet completed in Redis.

public:
    // Public so std::make_shared can construct the object in one allocation. Creation remains
    // funnelled through the factories above.
    /**
     * @brief Constructor.
     * @param clientSession     Client session.
     * @param message           Message.
     * @param qos               QoS.
     * @param deliveryId        Delivery id.
     * @param subscriptionIds   Matched subscription ids.
     * @param setRetainFlag     Set retain flag.
     */
    MessageDelivery(const std::shared_ptr<ClientSession>& clientSession, const SMessage& message, Qos qos,
                    const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, bool setRetainFlag);
};

/**
 * @brief Message delivery shared pointer
 */
using SMessageDelivery = std::shared_ptr<MessageDelivery>;

} // namespace xmq
