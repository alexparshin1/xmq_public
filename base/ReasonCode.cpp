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

#include "ReasonCode.h"

using namespace std;
using namespace xmq;

namespace {
constexpr const char* reasonCodeDescription(const ReasonCode reasonCode)
{
    const auto* description = "Success";
    using enum ReasonCode;
    switch (reasonCode)
    {
        case Success:
            break;
        case ErrorInvalidProtocol:
            description = "Invalid protocol";
            break;
        case ErrorIdentifierRejected:
            description = "Identifier rejected";
            break;
        case ErrorServerNotAvailable:
            description = "Server not available";
            break;
        case ErrorAuthenticationFailed:
            description = "Authentication failed";
            break;
        case ErrorNotAuthorized:
            description = "Not authorized";
            break;
        case UnspecifiedError:
            description = "Unspecified error";
            break;
        case MalformedPacket:
            description = "Malformed packet";
            break;
        case ProtocolError:
            description = "Protocol error";
            break;
        case ImplementationSpecificError:
            description = "Implementation specific error";
            break;
        case UnsupportedProtocolVersion:
            description = "Unsupported Protocol Version";
            break;
        case IdentifierRejected:
            description = "Client Identifier not valid";
            break;
        case AuthenticationFailed:
            description = "Bad user name or password";
            break;
        case NotAuthorized:
            description = "Client not authorized to connect";
            break;
        case ServerUnavailable:
            description = "The MQTT server unavailable";
            break;
        case ServerBusy:
            description = "Server busy";
            break;
        case Banned:
            description = "The client banned by administrative action";
            break;
        case BadAuthenticationMethod:
            description = "The authentication method is not supported";
            break;
        case WillTopicNameInvalid:
            description = "The will topic Name is not malformed, but is not accepted by this Server";
            break;
        case PacketTooLarge:
            description = "The CONNECT packet exceeded the maximum permissible size";
            break;
        case QuotaExceeded:
            description = "An implementation or administrative imposed limit has been exceeded";
            break;
        case PayloadFormatInvalid:
            description = "The will payload does not match the specified Payload Format Indicator";
            break;
        case RetainNotSupported:
            description = "The Server does not support retained messages, and Will Retain was set to 1";
            break;
        case WillQoSNotSupported:
            description = "The Server does not support the QoS set in Will QoS";
            break;
        case UseAnotherServer:
            description = "The Client should temporarily use another server";
            break;
        case ServerMoved:
            description = "The Client should permanently use another server";
            break;
        case ConnectionRateExceeded:
            description = "The Server has exceeded the connection rate limit";
            break;
    }
    return description;
}
} // namespace

string xmq::toString(const ReasonCode reasonCode)
{
    return reasonCodeDescription(reasonCode);
}
