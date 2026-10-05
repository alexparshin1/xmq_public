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

#include "MessageDeliveryThreads.h"

#include "base/LatencyTrace.h"

#include <cstdlib>

using namespace std;
using namespace sptk;
using namespace xmq;

MessageDeliveryThread::MessageDeliveryThread(const MessageDeliveryThreads& pool, const size_t threadIndex)
    : Thread("SubscriptionDelivery")
    , m_pool(pool)
    , m_threadIndex(threadIndex)
{
    Thread::run();
}

MessageDeliveryThread::~MessageDeliveryThread()
{
    Thread::terminate();
}

void MessageDeliveryThread::threadFunction()
{
    while (!terminated())
    {
        DeliveryTask task;
        if (!m_taskQueue.pop_front(task, 500ms) || !task.m_message)
        {
            // No message: the wake-up terminateThreads() queues.
            continue;
        }

        if (task.m_sessions)
        {
            // One shard of a fan-out already matched by another pool thread.
            SubscriptionManager::deliverShard(task.m_message, *task.m_sessions, task.m_shardIndex, task.m_shardCount);
            continue;
        }

        // If message is a trace, update trace.
        if (const auto messageProperties = task.m_message->getProperties())
        {
            Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ServerDelivery);
        }

        m_pool.fanOutMessage(task.m_message, m_threadIndex);
    }
}

MessageDeliveryThreads::MessageDeliveryThreads(SubscriptionManager& subscriptionManager, const size_t threadCount)
    : m_subscriptionManager(subscriptionManager)
{
    // Half the cores measured best: the shard threads compete with the send/receive threads that
    // do the socket I/O the fan-out produces, so using every core for delivery starves them.
    // XMQ_FANOUT_SHARDS overrides for benchmarking on other hardware; 1 disables sharding.
    m_shardCount = min(threadCount, max<size_t>(2, thread::hardware_concurrency() / 2));
    if (const char* shards = getenv("XMQ_FANOUT_SHARDS"))
    {
        m_shardCount = clamp<size_t>(strtoul(shards, nullptr, 10), 1, threadCount);
    }

    scoped_lock lock(m_mutex);
    for (size_t i = 0; i < threadCount; i++)
    {
        m_threads.push_back(make_unique<MessageDeliveryThread>(*this, i));
    }
}

MessageDeliveryThreads::~MessageDeliveryThreads()
{
    terminateThreads();

    scoped_lock lock(m_mutex);
    for (const auto& thread: m_threads)
    {
        thread->join();
    }
}

void MessageDeliveryThreads::terminateThreads() const
{
    scoped_lock lock(m_mutex);
    for (const auto& thread: m_threads)
    {
        thread->terminate();
        // An empty task wakes the thread at once, where it otherwise noticed only when its wait
        // timed out - up to half a second of every server stop.
        thread->enqueue(DeliveryTask {});
    }
}

void MessageDeliveryThreads::publishMessage(const SPublishMessage& message) const
{
    // Route by publisher session so all messages from one publisher are handled in order by a
    // single worker thread, preserving per-publisher delivery order [MQTT-4.6.0-6], while still
    // spreading different publishers across the pool.
    const auto threadIndex = std::hash<std::string> {}(message->getSender()) % m_threads.size();
    m_threads[threadIndex]->enqueue(DeliveryTask {.m_message = message});
}

void MessageDeliveryThreads::deliverNow(const SPublishMessage& message) const
{
    fanOutMessage(message, std::hash<std::string> {}(message->getSender()) % m_threads.size());
}

void MessageDeliveryThreads::fanOutMessage(const SPublishMessage& message, const size_t originThreadIndex) const
{
    // The match result is reused by every message this thread fans out, so that once it has grown
    // to the thread's largest fan-out it stops allocating. It used to be a make_shared per message,
    // for the sake of the sharded case alone - which now copies it instead, and only then.
    thread_local mqtt::SubscriptionIds sessions;

    // Its entries hold their sessions alive, so they are released when this message is done, not
    // when the thread's next message comes - which on a quiet thread could be never.
    struct ReleaseSessions
    {
        mqtt::SubscriptionIds& ids;

        ~ReleaseSessions()
        {
            ids.clear();
        }
    } releaseSessions {sessions};

    cluster::NodeSet bridges;

    // The delivery thread pool only handles client-domain messages; server-domain publishes
    // (LWT, $SYS) call SubscriptionManager::publishMessage directly.
    if (!m_subscriptionManager.matchMessage(message, MessageDomain::Client, sessions, bridges))
    {
        return;
    }

    if (const auto shardCount = m_shardCount;
        sessions.size() < MinSessionsForSharding || shardCount < 2)
    {
        SubscriptionManager::deliverShard(message, sessions, 0, 1);
    }
    else
    {
        // Count the sessions per shard first, so empty shards don't wake their threads.
        vector<uint32_t> shardSizes(shardCount, 0);
        for (const auto& [session, info]: sessions)
        {
            ++shardSizes[SubscriptionManager::shardOf(session, shardCount)];
        }

        // The origin thread keeps one shard for itself so it works in parallel with the others.
        // ownShard is stable per publisher (the publisher is pinned to the origin thread), so
        // per-publisher per-subscriber delivery stays on one thread and ordered.
        const auto ownShard = originThreadIndex % shardCount;
        // The other shards run on other threads after this one has moved on, so they get their own
        // copy - one allocation spread over a fan-out large enough to be worth sharding.
        const auto shared = make_shared<const mqtt::SubscriptionIds>(sessions);
        for (size_t shardIndex = 0; shardIndex < shardCount; ++shardIndex)
        {
            if (shardIndex == ownShard || shardSizes[shardIndex] == 0)
            {
                continue;
            }
            m_threads[shardIndex]->enqueue(DeliveryTask {.m_message = message,
                                                         .m_sessions = shared,
                                                         .m_shardIndex = static_cast<uint32_t>(shardIndex),
                                                         .m_shardCount = static_cast<uint32_t>(shardCount)});
        }

        SubscriptionManager::deliverShard(message, sessions, ownShard, shardCount);
    }

    for (auto& clusterNode: bridges)
    {
        clusterNode->publish(message);
    }
}

size_t MessageDeliveryThreads::queueLength() const
{
    size_t total = 0;
    for (const auto& thread: m_threads)
    {
        total += thread->queueLength();
    }
    return total;
}
