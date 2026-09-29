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

#include "MessageDelivery.h"
#include "ClientSession/ClientSession.h"
#include "Server.h"
#include "common/mqtt/PublishMessage.h"

#include <chrono>
#include <format>

using namespace std;
using namespace sptk;
using namespace xmq;

atomic<RecordId>   MessageDelivery::m_recordIdSerial {0};
atomic_size_t      MessageDelivery::m_queuedOperations; ///< Number of queued message deliveries not yet stored to Redis.
mutex              MessageDelivery::m_writeCapacityMutex;
condition_variable MessageDelivery::m_writeCapacityAvailable;
atomic_size_t      MessageDelivery::m_queuedWrites;
atomic_size_t      MessageDelivery::m_writeCapacityWaiters;
atomic_size_t      MessageDelivery::m_writeCapacityResumeAt;

mutex                             MessageDelivery::m_pausedSessionsMutex;
vector<weak_ptr<ClientSession>>   MessageDelivery::m_pausedSessions;
atomic_size_t                     MessageDelivery::m_pausedSessionCount;
atomic_size_t                     MessageDelivery::m_writeCapacityPauses;

mutex&                                          MessageDelivery::m_pendingWritesMutex = *new mutex;
condition_variable&                             MessageDelivery::m_pendingWritesAdded = *new condition_variable;
unordered_map<RecordId, MessageDelivery::PendingWrite>& MessageDelivery::m_pendingWrites = *new unordered_map<RecordId, MessageDelivery::PendingWrite>;
STimerEvent                       MessageDelivery::m_pauseTimeout;

namespace {

// Never wait forever on Redis: if writes stopped completing the broker should keep delivering,
// degraded, rather than stall every sending thread.
constexpr auto WriteCapacityTimeout = std::chrono::seconds(5);

// How long a record waits before it is written, in case its delivery finishes first. Long enough
// for a connected subscriber's PUBACK, which takes a few hundred microseconds; short enough that
// the records waiting at any moment - rate times this - stay well inside max_queued_writes.
constexpr auto WriteBehindDelay = std::chrono::milliseconds(1);

// 0 (or absent) means every delivery waits for its own record - the fully durable default.
size_t maxQueuedWritesSetting(const std::shared_ptr<ClientSession>& clientSession)
{
    const auto& settings = clientSession->server().getSettings();
    if (!settings)
    {
        return 0;
    }
    const auto& maxQueuedWrites = settings->m_persistence.m_max_queued_writes;
    if (maxQueuedWrites.isNull())
    {
        return 0;
    }
    const auto value = maxQueuedWrites.asInteger();
    return value > 0 ? static_cast<size_t>(value) : 0;
}

} // namespace

shared_ptr<MessageDelivery> MessageDelivery::create(const shared_ptr<ClientSession>& clientSession, const SMessage& message, const Qos qos,
                                                    const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, const bool setRetainFlag, const int64_t messageDeliveryRecordId)
{
    auto messageDelivery = make_shared<MessageDelivery>(clientSession, message, qos, deliveryId, subscriptionIds, setRetainFlag);
    if (qos != Qos::Qos0 && !clientSession->isCleanSession() && messageDeliveryRecordId == 0)
    {
        if (const auto maxQueuedWrites = maxQueuedWritesSetting(clientSession); maxQueuedWrites == 0)
        {
            // Fully durable: the record is in Redis before the message goes anywhere. Costs a
            // full round-trip per message, and since the thread is parked for it, the writes
            // never pipeline - this is what caps the broker at ~25,000 messages/s.
            Semaphore semaphore;
            messageDelivery->storeRecordAsync([&semaphore]
                                              {
                                                  semaphore.post();
                                              });
            semaphore.wait();
        }
        else
        {
            // Cached: let the delivery proceed while its record is written, so writes pipeline.
            // Up to maxQueuedWrites messages may then be in flight without a durable record and
            // would be lost if the server crashed - the setting is that window.
            messageDelivery->storeRecordBehind();
            awaitWriteCapacity(maxQueuedWrites);
        }
    }
    return messageDelivery;
}

void MessageDelivery::awaitWriteCapacity(const size_t maxQueuedWrites)
{
    // Called after the write was counted, so a backlog of exactly maxQueuedWrites is the window
    // full, not exceeded. A full window is the receive path's to handle - it pauses the publisher's
    // session before the next PUBLISH (see pauseUntilWriteCapacity()). Blocking here at a full
    // window would park the thread first, every time, and the pause would never be reached. What
    // is left for this is a PUBLISH whose own fan-out overran the window.
    if (m_queuedWrites.load() <= maxQueuedWrites)
    {
        return;
    }

    // Redis is behind. Block until the backlog has halved rather than releasing at the limit:
    // waking every waiter the moment one operation completes would leave the broker oscillating
    // at the threshold instead of draining.
    const auto resumeAt = maxQueuedWrites / 2;
    m_writeCapacityResumeAt.store(resumeAt);

    ++m_writeCapacityWaiters;
    {
        unique_lock lock(m_writeCapacityMutex);
        m_writeCapacityAvailable.wait_for(lock, WriteCapacityTimeout, [resumeAt]
                                          {
                                              return m_queuedWrites.load() <= resumeAt;
                                          });
    }
    --m_writeCapacityWaiters;
}

bool MessageDelivery::writeCapacityExhausted(const shared_ptr<ClientSession>& clientSession)
{
    const auto maxQueuedWrites = maxQueuedWritesSetting(clientSession);
    return maxQueuedWrites != 0 && m_queuedWrites.load(memory_order_relaxed) >= maxQueuedWrites;
}

void MessageDelivery::pauseUntilWriteCapacity(const shared_ptr<ClientSession>& clientSession)
{
    const auto maxQueuedWrites = maxQueuedWritesSetting(clientSession);
    const auto resumeAt = maxQueuedWrites / 2;
    m_writeCapacityResumeAt.store(resumeAt);
    m_writeCapacityPauses.fetch_add(1, memory_order_relaxed);

    {
        const scoped_lock lock(m_pausedSessionsMutex);
        m_pausedSessions.push_back(clientSession);
        m_pausedSessionCount.store(m_pausedSessions.size(), memory_order_relaxed);

        if (m_pausedSessions.size() == 1)
        {
            if (const auto timer = clientSession->server().getTimer())
            {
                m_pauseTimeout = timer->fireAt(DateTime::clock::now() + WriteCapacityTimeout, []
                                               {
                                                   resumePausedSessions();
                                               });
            }
        }
    }

    // The writes may all have completed between the caller's check and the session joining the
    // list, and then no completion is left to resume it.
    if (m_queuedWrites.load() <= resumeAt)
    {
        resumePausedSessions();
    }
}

void MessageDelivery::resumePausedSessions()
{
    vector<weak_ptr<ClientSession>> paused;
    STimerEvent                     timeout;
    {
        const scoped_lock lock(m_pausedSessionsMutex);
        paused.swap(m_pausedSessions);
        m_pausedSessionCount.store(0, memory_order_relaxed);
        timeout = std::move(m_pauseTimeout);
    }
    if (timeout)
    {
        timeout->cancel();
    }

    for (const auto& weakSession: paused)
    {
        if (const auto clientSession = weakSession.lock())
        {
            clientSession->resumeAfterWriteCapacity();
        }
    }
}

void MessageDelivery::releaseWriteCapacity()
{
    // Only broadcast when it can actually release someone. Notifying on every completed write
    // means tens of thousands of wakeups per second across all blocked delivery threads, and
    // that thundering herd costs far more than the round-trips it was meant to save.
    if (m_writeCapacityWaiters.load(memory_order_relaxed) == 0 && m_pausedSessionCount.load(memory_order_relaxed) == 0)
    {
        return;
    }
    if (m_queuedWrites.load(memory_order_relaxed) > m_writeCapacityResumeAt.load(memory_order_relaxed))
    {
        return;
    }
    if (m_pausedSessionCount.load(memory_order_relaxed) != 0)
    {
        resumePausedSessions();
    }
    m_writeCapacityAvailable.notify_all();
}

std::shared_ptr<MessageDelivery> MessageDelivery::create(const std::shared_ptr<ClientSession>& clientSession, const SMessage& message, const Qos qos, const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, const bool setRetainFlag, const int64_t messageDeliveryRecordId, const function<void(const shared_ptr<MessageDelivery>&)>& completionCallback)
{
    auto messageDelivery = make_shared<MessageDelivery>(clientSession, message, qos, deliveryId, subscriptionIds, setRetainFlag);
    if (qos != Qos::Qos0 && !clientSession->isCleanSession() && messageDeliveryRecordId == 0)
    {
        messageDelivery->storeRecordAsync([messageDelivery, completionCallback]
                                          {
                                              completionCallback(messageDelivery);
                                          });
    }
    return messageDelivery;
}

MessageDelivery::MessageDelivery(const std::shared_ptr<ClientSession>& clientSession, const SMessage& message, const Qos qos,
                                 const MessageId& deliveryId, const SubscriptionIdSet& subscriptionIds, const bool setRetainFlag)
    : MessageDispatch(message, qos, deliveryId, setRetainFlag, subscriptionIds)
    , PersistentObject(clientSession->getRedis())
    , m_clientSession(clientSession)
    , m_recordId(++m_recordIdSerial)
{
    // The key is only used by Redis persistence. In the in-memory broker it would allocate a
    // string for every delivered message, even though no record is ever written.
    if (getRedis())
    {
        m_sessionMessagesKey = std::format("session_{}_messages", clientSession->getClientIdUnlocked());
    }
}

MessageDelivery::MessageDelivery(const std::shared_ptr<ClientSession>& clientSession, const Buffer& packedData)
    : PersistentObject(clientSession->getRedis())
    , m_clientSession(clientSession)
    , m_sessionMessagesKey(std::format("session_{}_messages", clientSession->getClientIdUnlocked()))
{
    MessageDelivery::unpack(packedData);
    // The packed data came from Redis, and unpack() restored its record id.
    m_persisted = true;

    // Anything restored from Redis may already have been sent to the client before the server
    // stopped: the record carries no "delivery attempted" state, and adding one would cost
    // another Redis write per message. Flag every restored delivery as a duplicate, which is the
    // safe direction - MQTT 3.1.1 3.3.1.1 says a receiver of DUP=1 "cannot assume that it has
    // seen an earlier copy of this packet", while sending DUP=0 for a message the client already
    // received is a protocol violation. Deduplication is by packet identifier, not by this flag.
    m_flags.setDup(true);
}

MessageDelivery::~MessageDelivery()
{
    // Only remove records that actually exist in Redis: deliveries that were never persisted
    // (QoS 0, clean sessions) must not queue a delete operation per destroyed message.
    // If Redis is not connected, the record is not removed.
    if (m_persisted)
    {
        // Still waiting to be written: then it never will be, and there is nothing to delete.
        // Decided under the lock the flusher sends under, so a record is either dropped here
        // or already queued on this connection ahead of the HDEL below - never neither.
        bool dropped = false;
        {
            const scoped_lock lock(m_pendingWritesMutex);
            dropped = m_pendingWrites.erase(m_recordId) != 0;
        }
        if (dropped)
        {
            --m_queuedOperations;
            --m_queuedWrites;
            releaseWriteCapacity();
            return;
        }
        removeRecordAsync({});
    }
}

void MessageDelivery::storeRecordBehind()
{
    const auto redis = getRedis();
    if (!redis || !redis->isConnected())
    {
        return;
    }

    static once_flag flusherStarted;
    call_once(flusherStarted, []
              {
                  thread(runWriteFlusher).detach();
              });

    Buffer record;
    pack(record);
    m_persisted = true;
    ++m_queuedOperations;
    ++m_queuedWrites;
    {
        const scoped_lock lock(m_pendingWritesMutex);
        m_pendingWrites.emplace(m_recordId, PendingWrite {redis, m_sessionMessagesKey, to_string(m_recordId), std::move(record)});
    }
    m_pendingWritesAdded.notify_one();
}

void MessageDelivery::runWriteFlusher()
{
    while (true)
    {
        {
            unique_lock lock(m_pendingWritesMutex);
            m_pendingWritesAdded.wait(lock, []
                                      {
                                          return !m_pendingWrites.empty();
                                      });
        }
        // Everything queued in the meantime rides along, so under load this is one flush per
        // WriteBehindDelay, not one per record.
        this_thread::sleep_for(WriteBehindDelay);
        flushPendingWrites();
    }
}

void MessageDelivery::flushPendingWrites()
{
    const scoped_lock lock(m_pendingWritesMutex);
    flushPendingWritesLocked();
}

void MessageDelivery::flushPendingWritesLocked()
{
    for (auto& [recordId, write]: m_pendingWrites)
    {
        write.redis->setHashValueAsync(write.key, write.field, Variant(std::move(write.record)),
                                       []
                                       {
                                           --m_queuedOperations;
                                           --m_queuedWrites;
                                           releaseWriteCapacity();
                                       });
    }
    m_pendingWrites.clear();
}

void MessageDelivery::pack(Buffer& record)
{
    const auto publishMessage = static_cast<mqtt::PublishMessage*>(m_message.get());

    startPacking(record);

    const auto expectedSizeWithoutProperties =
        sizeof(m_recordId) + 1                                                                      // recordId
        + sizeof(uint8_t) + 1                                                                       // flags
        + sizeof(uint16_t) + 1 + sizeof(uint32_t) * m_subscriptionIds.size() + 1 + sizeof(uint32_t) // subscriptionIds
        + sizeof(uint32_t) + 1 + publishMessage->destination()->fullName().length()                 // topicName
        + sizeof(uint32_t) + 1 + publishMessage->payloadSize()                                      // payload
        + sizeof(uint32_t) + 1 + publishMessage->getSender().size()                                 // sender
        + sizeof(uint32_t) + 1 + publishMessage->getSourceNode().size()                             // sourceNode
        + sizeof(m_deliveryId) + 1                                                                  // deliveryId
        + 64;                                                                                       // minimal reserve for properties

    record.reserve(expectedSizeWithoutProperties);

    write(m_recordId);
    write(static_cast<uint8_t>(m_flags));

    const auto subscriptionIdCount = static_cast<uint16_t>(m_subscriptionIds.size());
    write(subscriptionIdCount);
    write(reinterpret_cast<uint8_t*>(m_subscriptionIds.data()), subscriptionIdCount * sizeof(uint32_t));

    write(publishMessage->destination()->fullName());
    write(publishMessage->payloadData(), publishMessage->payloadSize());
    write(publishMessage->getSender());
    write(publishMessage->getSourceNode());

    const uint8_t hasProperties = publishMessage->getProperties() != nullptr;
    write(hasProperties);
    if (hasProperties)
    {
        write(*publishMessage->getProperties());
    }

    // Appended last, so that records written before the delivery id was persisted still parse:
    // unpack() reads it only if the record actually extends this far.
    write(m_deliveryId);
}

void MessageDelivery::unpack(const Buffer& record)
{
    startUnpacking(string_view(record.c_str(), record.size()));
    read(m_recordId);
    if (m_recordId > m_recordIdSerial.load())
    {
        m_recordIdSerial = m_recordId + 1;
    }
    read(*reinterpret_cast<uint8_t*>(&m_flags));

    uint16_t subscriptionCount {0};
    read(subscriptionCount);
    m_subscriptionIds.resize(subscriptionCount);
    read(reinterpret_cast<uint8_t*>(m_subscriptionIds.data()), subscriptionCount * sizeof(uint32_t));

    string destination;
    read(destination);
    const auto* topic = m_clientSession->server().getTopic(destination);

    Buffer payload;
    read(payload);

    string sender;
    read(sender);

    string sourceNode;
    read(sourceNode);

    uint8_t hasProperties;
    read(hasProperties);

    shared_ptr<MessageProperties> properties;
    if (hasProperties)
    {
        properties = make_shared<MessageProperties>();
        read(*properties);
    }

    // Written by newer versions only. Left at 0 for records that predate it, which restore then
    // treats as "no original id known" and allocates a fresh one for.
    if (hasMoreData())
    {
        read(m_deliveryId);
    }

    const auto publishMessage = make_shared<mqtt::PublishMessage>(topic, string(payload.c_str(), payload.size()));
    publishMessage->setSender(sender);
    publishMessage->setSourceNode(sourceNode);
    if (hasProperties)
    {
        publishMessage->setProperties(properties);
    }
    m_message = publishMessage;
}

void MessageDelivery::storeRecordAsync(const function<void()>& callback)
{
    const auto redis = getRedis();
    if (!redis || !redis->isConnected())
    {
        // Nothing to persist, but the callback still has to run: callers treat it as
        // "the record is safe, carry on", and the synchronous create() blocks on a
        // semaphore until it fires. Skipping it deadlocked every QoS1/QoS2 delivery
        // to a persistent session while persistence was disabled.
        if (callback)
        {
            callback();
        }
        return;
    }

    Buffer record;
    pack(record);
    m_persisted = true;
    ++m_queuedOperations;
    ++m_queuedWrites;
    redis->setHashValueAsync(m_sessionMessagesKey, to_string(m_recordId), record,
                             [callback]
                             {
                                 --m_queuedOperations;
                                 --m_queuedWrites;
                                 releaseWriteCapacity();
                                 if (callback)
                                 {
                                     callback();
                                 }
                             });
}

void MessageDelivery::removeRecordAsync(const std::function<void(const size_t&)>& callback)
{
    // Called from the destructor, possibly while the calling thread holds the session's
    // write lock, so it must not acquire any ClientSession locks (see m_sessionMessagesKey).
    const auto redis = getRedis();
    if (!redis || !redis->isConnected())
    {
        // No record to remove, but the callback must still run - see storeRecordAsync().
        if (callback)
        {
            callback(0);
        }
        return;
    }
    ++m_queuedOperations;
    redis->deleteHashKeysAsync(m_sessionMessagesKey, {to_string(m_recordId)},
                               [callback]
                               {
                                   --m_queuedOperations;
                                   if (callback)
                                   {
                                       callback(1);
                                   }
                               });
}
