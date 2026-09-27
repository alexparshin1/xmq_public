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

#include "base/Message.h"
#include "common/mqtt/PublishMessage.h"

namespace xmq {

class MessageDispatch
{
public:
    SMessage          m_message;         ///< Message to send.
    MessageId         m_deliveryId {0};  ///< Message id.
    MessageFlags      m_flags;           ///< Delivery message flags: isRetain, duplicated, getQos.
    SubscriptionIdSet m_subscriptionIds; ///< Subscription ids.

    /**
     * @brief Default constructor.
     */
    MessageDispatch() = default;

    /**
     * @brief Copy constructor.
     */
    MessageDispatch(const MessageDispatch& other) = default;

    /**
     * @brief Constructor.
     */
    MessageDispatch(const SMessage& message, Qos qos, MessageId messageId = 0, bool duplicated = false, bool setRetainFlag = false);

    /**
     * @brief Constructor
     */
    MessageDispatch(const SMessage& message, Qos qos, MessageId messageId, bool setRetainFlag, const SubscriptionIdSet& subscriptionIds);

    /**
     * @brief Destructor
     */
    virtual ~MessageDispatch() = default;

    [[nodiscard]] SMessage getMessage() const
    {
        return m_message;
    }

    [[nodiscard]] mqtt::SPublishMessage getPublishMessage() const
    {
        return std::dynamic_pointer_cast<mqtt::PublishMessage>(m_message);
    }
};

using SMessageDispatch = std::shared_ptr<MessageDispatch>;
using UMessageDispatch = std::unique_ptr<MessageDispatch>;

} // namespace xmq
