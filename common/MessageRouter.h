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

namespace xmq {

class MessageRouter
{
public:
    /**
     * @brief Constructor.
     */
    MessageRouter() = default;
    /**
     * @brief Destructor.
     */
    virtual ~MessageRouter() = default;

    void route(SMessage&& sharedMessage);

    virtual void receivedAck(const MessageId& messageId, Message::Type messageType) = 0;

protected:
    virtual void handleConnectMessage(const SMessage& message) = 0;
    virtual void handleSubscribeMessage(const Message* message) = 0;
    virtual void handleUnsubscribeMessage(const Message* message) = 0;
    virtual void handleDisconnect(const Message* message) = 0;

    virtual void ackPublishMessage(const Message* message, ReasonCode reasonCode) = 0;
    virtual void handlePublishMessage(const SMessage& message) = 0;
    virtual void handlePublishReleaseMessage(const Message* message) = 0;

    virtual void handlePingReq(const Message* message) = 0;
};

} // namespace xmq
