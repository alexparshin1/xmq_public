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

#include "BaseProtocolVersion.h"
#include "ConnectMessage.h"
#include "PublishMessage.h"
#include "SubscribeMessage.h"
#include "UnsubscribeMessage.h"
#include "base/Message.h"
#include "base/MessageProperties.h"
#include "base/SubscriptionIdSet.h"

namespace xmq {

/**
 * @brief The tweak message callback type.
 * @details Used for testing purposes if a message has to be tweaked.
 * @param message Message buffer before sending.
 */
using TweakMessageCallback = std::function<void(sptk::Buffer& message)>;

/**
 * @brief Generic message writer.
 */
class XMQ_EXPORT MessageWriter : public BaseProtocolVersion
{
public:
    /**
     * @brief Constructor.
     */
    explicit MessageWriter(ProtocolVersion protocolVersion);

    MessageWriter(const MessageWriter&) = delete;
    MessageWriter(MessageWriter&&) = delete;
    MessageWriter& operator=(const MessageWriter&) = delete;
    MessageWriter& operator=(MessageWriter&&) = delete;

    virtual ~MessageWriter() = default;

    /**
     * @brief Write a message into the buffer.
     * @param buffer            Output buffer.
     * @param message           Message.
     * @param messageFlags      Message delivery flags.
     * @param deliveryId        Message id.
     * @param remainingExpirationSeconds Remaining expiration seconds.
     * @param subscriptionIds   Matched subscriptions ids.
     * @param maxPacketSize     Maximum packet size.
     */
    virtual bool appendMessageToBuffer(sptk::Buffer& buffer, const Message& message, MessageFlags messageFlags, const MessageId& deliveryId,
                                       uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds, size_t maxPacketSize) const = 0;

    /**
     * @brief Write the Connect message.
     * @param messageBuffer     Message buffer.
     * @param connectMessage    Connect message.
     * @param tweakMessage      Tweak message callback (for testing purposes).
     */
    virtual void writeConnect(sptk::Buffer& messageBuffer, const ConnectMessage* connectMessage, const TweakMessageCallback& tweakMessage) const = 0;

    /**
     * @brief Append a 'publish' message to the buffer.
     * @param buffer            Message buffer.
     * @param message           Publish message.
     * @param messageFlags      Message delivery flags.
     * @param deliveryId        Message id.
     * @param remainingExpirationSeconds Remaining expiration seconds.
     * @param subscriptionIds   Matched subscriptions ids.
     * @param maxPacketSize     Maximum packet size.
     */
    virtual bool appendPublishToBuffer(sptk::Buffer& buffer, const PublishMessage& message, MessageFlags messageFlags,
                                       const MessageId& deliveryId, uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                                       size_t maxPacketSize) const = 0;

    /**
     * @brief Append the Subscribe message to the buffer.
     * @param messageBuffer     Session socket.
     * @param message           Subscribe message.
     * @param messageId         Session's delivery id.
     */
    virtual void appendSubscribeToBuffer(sptk::Buffer& messageBuffer, const SubscribeMessage& message, MessageId messageId) const = 0;

    /**
     * @brief Write the Unsubscribe message to the buffer.
     * @param messageBuffer     Message buffer.
     * @param message           Unsubscribe message.
     * @param messageId         Session's delivery id.
     */
    virtual void appendUnsubscribeToBuffer(sptk::Buffer& messageBuffer, const UnsubscribeMessage& message, MessageId messageId) const = 0;

    /**
     * @brief Send ack.
     * @param messageBuffer     Session socket.
     * @param frameType         Frame type.
     * @param ackMessageId      Message id.
     * @param reasonCode        Reason code.
     */
    virtual void appendAckToBuffer(sptk::Buffer& messageBuffer, int frameType, const MessageId& ackMessageId, ReasonCode reasonCode) const = 0;

    /**
     * @brief Send ack.
     * @param messageBuffer     Session socket.
     * @param message           Message to which ack responds.
     * @param reasonCode        Reason code.
     */
    virtual void appendAckToBuffer(sptk::Buffer& messageBuffer, const Message* message, ReasonCode reasonCode) const = 0;

    /**
     * @brief Write the 'connect ack' to the buffer.
     * @param messageBuffer     Message buffer.
     * @param connectProperties Connect properties.
     * @param reasonCode        Reason code.
     * @param sessionPresent    If true then session already existed.
     */
    virtual void appendConnectAckToBuffer(sptk::Buffer& messageBuffer, const SMessageProperties& connectProperties, ReasonCode reasonCode, bool sessionPresent) const = 0;

    /**
     * @brief Write the 'ping request' to the buffer.
     * @param messageBuffer     Message buffer.
     */
    virtual void appendPingRequestToBuffer(sptk::Buffer& messageBuffer) const = 0;

    /**
     * @brief Write the 'ping response' to the buffer.
     * @param messageBuffer     Message buffer.
     */
    virtual void appendPingResponseToBuffer(sptk::Buffer& messageBuffer) const = 0;

    /**
     * @brief Write the Disconnect message to the buffer.
     * @param messageBuffer     Message buffer.
     * @param reasonCode        Reason code.
     */
    virtual void appendDisconnectToBuffer(sptk::Buffer& messageBuffer, ReasonCode reasonCode) const = 0;
};

/**
 * @brief Message writer's shared pointer.
 */
using SMessageWriter = std::shared_ptr<MessageWriter>;

} // namespace xmq
