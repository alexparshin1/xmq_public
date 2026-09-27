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

#include "MessageRouter.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void MessageRouter::route(SMessage&& sharedMessage)
{
    switch (const auto* message = sharedMessage.get();
            message->type())
    {
        using enum Message::Type;

        case Publish:
            ackPublishMessage(message, ReasonCode::Success);
            handlePublishMessage(sharedMessage);
            break;

        case PublishAck:
        case PublishReceived:
        case PublishComplete:
        case UnsubscribeAck:
        case PingResp:
            receivedAck(message->getId(), message->type());
            break;

        case PublishRelease:
            handlePublishReleaseMessage(message);
            break;

        case Connect:
            handleConnectMessage(sharedMessage);
            break;

        case Subscribe:
            handleSubscribeMessage(message);
            break;

        case Unsubscribe:
            handleUnsubscribeMessage(message);
            break;

        case ConnectAck:
        case SubscribeAck:
            break;

        case Disconnect:
            handleDisconnect(message);
            break;

        case PingReq:
            handlePingReq(message);
            break;

        case Undefined:
            throw Exception("Unsupported message type " + Message::messageTypeName(message->type()));
    }
}
