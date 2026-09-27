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

#include "MessageQueue.h"
#include "AckMessage.h"

#include <ranges>

using namespace std;
using namespace sptk;

using namespace xmq;

MessageQueue::MessageQueue(const uint16_t maxInflightMessages, ForwardMessage forwardMessage)
    : m_maxInflightMessages(maxInflightMessages > 0 ? maxInflightMessages : 32768)
    , m_forwardMessage(std::move(forwardMessage))
{
    if (m_forwardMessage == nullptr)
    {
        throw invalid_argument("forwardMessage is null");
    }
}

MessageQueue::~MessageQueue()
{
    const scoped_lock lock(m_mutex);
    m_waitingMessages.reset();
    m_inflightMessages.clear();
}

MessageQueue::WaitingMessages& MessageQueue::waitingMessages()
{
    if (!m_waitingMessages)
    {
        m_waitingMessages = make_unique<WaitingMessages>();
    }
    return *m_waitingMessages;
}

size_t MessageQueue::waitingCount() const
{
    return m_waitingMessages ? m_waitingMessages->size() : 0;
}

void MessageQueue::setMaxInflightMessages(const uint16_t maxInflightMessages)
{
    m_maxInflightMessages = maxInflightMessages > 0 ? maxInflightMessages : 32768;
}

MessageId MessageQueue::queueMessage(const SMessageDispatch& messageDispatch)
{
    const scoped_lock lock(m_mutex);
    return queueMessageUnlocked(messageDispatch);
}

MessageId MessageQueue::restoreMessage(SMessageDispatch& messageDispatch)
{
    const scoped_lock lock(m_mutex);
    return restoreMessageUnlocked(messageDispatch);
}

MessageId MessageQueue::nextDeliveryId()
{
    const scoped_lock lock(m_mutex);
    return nextDeliveryIdUnlocked();
}

MessageId MessageQueue::nextDeliveryIdUnlocked()
{
    ++m_nextMessageId;
    if (m_nextMessageId == 0)
    {
        // 0 is not a valid MQTT packet identifier.
        m_nextMessageId = 1;
    }
    return m_nextMessageId;
}

MessageId MessageQueue::queueMessageUnlocked(const SMessageDispatch& messageDispatch)
{
    if (messageDispatch->m_flags.getQos() == Qos::Qos0)
    {
        m_forwardMessage(messageDispatch);
        return 0;
    }

    // Keep an id the caller already allocated: it is the one under which this delivery's record
    // was persisted, and reassigning here would put a different id on the wire.
    if (messageDispatch->m_deliveryId == 0)
    {
        messageDispatch->m_deliveryId = nextDeliveryIdUnlocked();
    }

    const auto messageId = messageDispatch->m_deliveryId;

    if (m_maxInflightMessages > 0 && m_inflightMessages.size() >= m_maxInflightMessages)
    {
        waitingMessages().push_back(messageDispatch);
        return messageId;
    }

    m_forwardMessage(messageDispatch);

    m_inflightMessages[messageId] = messageDispatch;

    return messageId;
}

MessageId MessageQueue::restoreMessageUnlocked(SMessageDispatch& messageDispatch)
{
    if (messageDispatch->m_flags.getQos() == Qos::Qos0)
    {
        constexpr MessageId messageId = 0;
        return messageId;
    }

    if (messageDispatch->m_deliveryId == 0)
    {
        // A record written before the delivery id was persisted. Nothing to preserve, so fall back
        // to a fresh id - the client cannot match a late ack to it either way.
        messageDispatch->m_deliveryId = nextDeliveryIdUnlocked();
    }
    else if (messageDispatch->m_deliveryId > m_nextMessageId)
    {
        // Keep the allocator above every id already outstanding, so a message queued later cannot
        // be handed an id the client is still expecting to acknowledge.
        m_nextMessageId = messageDispatch->m_deliveryId;
    }

    // Captured before the move: reading through messageDispatch afterwards is undefined.
    const auto messageId = messageDispatch->m_deliveryId;

    waitingMessages().push_back(std::move(messageDispatch));

    return messageId;
}

void MessageQueue::receiveAck(MessageId messageId, Message::Type ackType)
{
    const scoped_lock lock(m_mutex);

    if (ackType == Message::Type::Publish)
    {
        const auto ackMessage = make_shared<AckMessage>(Message::Type::PublishAck, messageId);
        const auto messageDispatch = make_shared<MessageDispatch>(ackMessage, Qos::Qos0, messageId);
        if (m_forwardMessage)
        {
            m_forwardMessage(messageDispatch);
        }
    }

    if (ackType == Message::Type::PublishReceived)
    {
        auto       ackMessage = make_shared<AckMessage>(Message::Type::PublishRelease, messageId);
        const auto messageDispatch = make_shared<MessageDispatch>(ackMessage, Qos::Qos1, messageId);
        if (m_forwardMessage)
        {
            m_forwardMessage(messageDispatch);
        }
        return;
    }

    if (ackType == Message::Type::PublishRelease)
    {
        auto       ackMessage = make_shared<AckMessage>(Message::Type::PublishComplete, messageId);
        const auto messageDispatch = make_shared<MessageDispatch>(ackMessage, Qos::Qos1, messageId);
        if (m_forwardMessage)
        {
            m_forwardMessage(messageDispatch);
        }
        return;
    }

    if (const auto erasedCount = m_inflightMessages.erase(messageId);
        erasedCount == 0)
    {
        CERR("Unexpected ACK id=" << messageId << " type=" << static_cast<int>(ackType));
        return;
    }

    if (waitingCount() != 0)
    {
        auto& messageDispatch = m_waitingMessages->front();
        m_forwardMessage(messageDispatch);

        const auto deliveryId = messageDispatch->m_deliveryId;
        m_inflightMessages.try_emplace(deliveryId, std::move(messageDispatch));

        m_waitingMessages->pop_front();
    }
}

void MessageQueue::clear()
{
    const scoped_lock lock(m_mutex);

    m_inflightMessages.clear();
    m_waitingMessages.reset();
}

void MessageQueue::rescheduleMessagesWaitingForAck()
{
    const scoped_lock lock(m_mutex);

    if (m_inflightMessages.empty())
    {
        return;
    }

    auto& waiting = waitingMessages();
    for (auto& messageDispatch: views::values(m_inflightMessages))
    {
        waiting.push_back(std::move(messageDispatch));
    }
    m_inflightMessages.clear();
}

void MessageQueue::sendWaitingMessages(const ForwardMessage& forwardMessage)
{
    const scoped_lock lock(m_mutex);

    vector<SMessageDispatch> waitingMessages;
    waitingMessages.reserve(m_inflightMessages.size());

    MessageDispatchMap inflight;
    inflight.swap(m_inflightMessages);

    for (const auto& messageDispatch: views::values(inflight))
    {
        queueMessageUnlocked(messageDispatch);
    }

    while (waitingCount() != 0)
    {
        if (m_maxInflightMessages > 0 && m_inflightMessages.size() >= m_maxInflightMessages)
        {
            break;
        }
        auto messageDispatch = std::move(m_waitingMessages->front());
        m_waitingMessages->pop_front();
        forwardMessage(messageDispatch);
        m_inflightMessages[messageDispatch->m_deliveryId] = std::move(messageDispatch);
    }
}

vector<SMessageDispatch> MessageQueue::enqueuedMessages() const
{
    const scoped_lock lock(m_mutex);

    std::vector<SMessageDispatch> result;
    result.reserve(m_inflightMessages.size() + waitingCount());

    std::ranges::transform(m_inflightMessages, back_inserter(result),
                           [](const auto& pair)
                           {
                               return make_shared<MessageDispatch>(*pair.second);
                           });

    if (m_waitingMessages)
    {
        std::ranges::transform(*m_waitingMessages, back_inserter(result),
                               [](const auto& dispatch)
                               {
                                   return make_shared<MessageDispatch>(*dispatch);
                               });
    }

    return result;
}

size_t MessageQueue::size() const
{
    const scoped_lock lock(m_mutex);
    return m_inflightMessages.size() + waitingCount();
}

size_t MessageQueue::removeExpiredMessages(uint32_t& secondsUntilNextExpiration, const bool includeUnacknowledged)
{
    const scoped_lock lock(m_mutex);

    secondsUntilNextExpiration = 0;

    const auto isExpired = [](const SMessageDispatch& messageDispatch)
    {
        uint32_t remainingSeconds = 0;
        return messageDispatch->m_message->isExpired(remainingSeconds);
    };

    const auto sizeBefore = waitingCount() + m_inflightMessages.size();

    if (m_waitingMessages)
    {
        erase_if(*m_waitingMessages, isExpired);
    }

    if (includeUnacknowledged)
    {
        erase_if(m_inflightMessages,
                 [&isExpired](const auto& pair)
                 {
                     return isExpired(pair.second);
                 });
    }

    // Taken from what is left, so the caller can come back exactly when the next one is due
    // rather than polling. isExpired() reports the remaining seconds, and leaves them at zero for
    // a message with no expiry at all - which is unambiguous here, since a message whose expiry
    // had already arrived would have been reported as expired instead.
    uint32_t   soonestSeconds = 0;
    const auto trackSoonest = [&soonestSeconds](const SMessageDispatch& messageDispatch)
    {
        uint32_t remainingSeconds = 0;
        if (messageDispatch->m_message->isExpired(remainingSeconds))
        {
            return;
        }
        if (remainingSeconds != 0 && (soonestSeconds == 0 || remainingSeconds < soonestSeconds))
        {
            soonestSeconds = remainingSeconds;
        }
    };

    if (m_waitingMessages)
    {
        ranges::for_each(*m_waitingMessages, trackSoonest);
    }
    ranges::for_each(views::values(m_inflightMessages), trackSoonest);

    // One second past the interval: expiry is carried in whole seconds, so coming back exactly on
    // the boundary can land a moment early and find nothing to drop.
    secondsUntilNextExpiration = soonestSeconds != 0 ? soonestSeconds + 1 : 0;

    return sizeBefore - (waitingCount() + m_inflightMessages.size());
}
