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

#include "Message.h"

using namespace xmq;

std::string Message::messageTypeName(const Type type)
{
    switch (type)
    {
        using enum Type;
        case Connect:
            return "Connect";
        case Disconnect:
            return "Disconnect";
        case Subscribe:
            return "Subscribe";
        case Unsubscribe:
            return "Unsubscribe";
        case PingReq:
            return "PingReq";
        case Publish:
            return "Publish";
        case ConnectAck:
            return "ConnectAck";
        case SubscribeAck:
            return "SubscribeAck";
        case PublishAck:
            return "PublishAck";
        case PublishReceived:
            return "PublishReceived";
        case PublishRelease:
            return "PublishRelease";
        case PublishComplete:
            return "PublishComplete";
        case UnsubscribeAck:
            return "UnsubscribeAck";
        case PingResp:
            return "PingResp";
        default:
            return "Undefined";
    }
}

Message::Type Message::messageType(const std::string_view typeName)
{
    using enum Type;
    static const std::unordered_map<std::string_view, Type> typeMap = {
        {"Connect", Connect},
        {"Disconnect", Disconnect},
        {"Subscribe", Subscribe},
        {"Unsubscribe", Unsubscribe},
        {"PingReq", PingReq},
        {"Publish", Publish},
        {"ConnectAck", ConnectAck},
        {"SubscribeAck", SubscribeAck},
        {"PublishAck", PublishAck},
        {"PublishReceived", PublishReceived},
        {"PublishRelease", PublishRelease},
        {"PublishComplete", PublishComplete},
        {"UnsubscribeAck", UnsubscribeAck},
        {"PingResp", PingResp},
    };


    if (const auto it = typeMap.find(typeName);
        it != typeMap.end())
    {
        return it->second;
    }

    return Undefined;
}

std::ostream& operator<<(std::ostream& os, const Message& message)
{
    os << message.toString();
    return os;
}
