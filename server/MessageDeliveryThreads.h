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

#include "Subscription/SubscriptionManager.h"
#include <common/PublishMessage.h>

namespace xmq {

class MessageDeliveryThreads;

/**
 * @brief A unit of fan-out work processed by a MessageDeliveryThread.
 *
 * Without m_sessions the task is a freshly published message: the thread matches it against the
 * subscriptions and delivers it (possibly sharding a large fan-out across the pool). With
 * m_sessions the task is one shard of an already matched fan-out: the thread delivers the message
 * to the sessions of shard m_shardIndex only.
 */
struct DeliveryTask
{
    SPublishMessage                              m_message;
    std::shared_ptr<const mqtt::SubscriptionIds> m_sessions;
    uint32_t                                     m_shardIndex {0};
    uint32_t                                     m_shardCount {1};
};

class MessageDeliveryThread final : public sptk::Thread
{
public:
    /**
     * @brief Constructor.
     */
    MessageDeliveryThread(const MessageDeliveryThreads& pool, size_t threadIndex);

    /**
     * @brief Destructor.
     */
    ~MessageDeliveryThread() override;

    /**
     * @brief Enqueue a fan-out task for asynchronous processing by this thread.
     *
     * All messages from the same publisher must be enqueued to the same thread so that they are
     * delivered to subscribers in the publishing order [MQTT-4.6.0-6]. Shard tasks of one message
     * must be enqueued before shard tasks of the publisher's next message for the same reason.
     */
    void enqueue(DeliveryTask&& task)
    {
        m_taskQueue.push_back(std::move(task));
    }

protected:
    void threadFunction() override;

private:
    const MessageDeliveryThreads&         m_pool;
    const size_t                          m_threadIndex;
    sptk::SynchronizedQueue<DeliveryTask> m_taskQueue;

public:
    /// Tasks waiting in this worker's queue; summed by MessageDeliveryThreads::queueLength().
    [[nodiscard]] size_t queueLength() const
    {
        return m_taskQueue.size();
    }
};

class MessageDeliveryThreads final
{
    friend class MessageDeliveryThread;

public:
    /**
     * @brief Constructor.
     */
    MessageDeliveryThreads(SubscriptionManager& subscriptionManager, size_t threadCount);

    /**
     * @brief Destructor.
     */
    ~MessageDeliveryThreads();

    void terminateThreads() const;

    /**
     * @brief Asynchronously fan-out a message to subscribers.
     *
     * The message is routed to a worker thread chosen by hashing the publisher session. The
     * publishing connection receive thread is not blocked by the delivery work, while messages
     * from a given publisher are still processed in order by a single worker.
     */
    void publishMessage(const SPublishMessage& message) const;

    /**
     * @brief Match and deliver a message on the calling thread instead of handing it to a worker.
     *
     * For a receive thread when Server::deliversOnReceiveThread() allows it. The publisher keeps the
     * shard it would have had, so a fan-out is split across the pool as before.
     */
    void deliverNow(const SPublishMessage& message) const;

    /**
     * @brief Total number of delivery tasks waiting across the pool.
     *
     * Read once a second by the metrics scan: each queue's size() takes the same mutex its push
     * and pop take, and this pool's queues are hot.
     */
    [[nodiscard]] size_t queueLength() const;

private:
    /**
     * @brief Match a message and deliver it to the subscribers, on a worker thread or through deliverNow().
     *
     * A fan-out below MinSessionsForSharding is delivered inline. A larger fan-out is split into
     * one shard per pool thread: the other threads' shards are enqueued to them, then the calling
     * thread delivers its own shard inline. The session-to-shard mapping is a stable hash with the
     * shard count fixed at the pool size, so consecutive messages to the same subscriber take the
     * same thread and stay ordered.
     */
    void fanOutMessage(const SPublishMessage& message, size_t originThreadIndex) const;

    /// Minimal matched-session count for splitting the fan-out across the pool. Below it, the
    /// shard enqueue and thread wake-up overhead costs more than the serialized delivery loop.
    /// When subscription churn moves a topic across this threshold between two messages, the
    /// second message switches between the inline and the sharded path and can overtake the
    /// first for a subscriber; the window is one in-flight fan-out.
    static constexpr size_t MinSessionsForSharding = 32;

    mutable std::mutex                                  m_mutex;
    SubscriptionManager&                                m_subscriptionManager;
    size_t                                              m_shardCount {1};
    std::vector<std::unique_ptr<MessageDeliveryThread>> m_threads;
};

} // namespace xmq
