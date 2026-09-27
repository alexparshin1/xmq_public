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

#include "xmq.h"

#include <cstdint>
#include <string>

namespace xmq {

/**
* @brief Reason code.
*/
enum class ReasonCode : uint8_t
{
    Success = 0,

    // MQTT 3 Error Codes
    ErrorInvalidProtocol = 1,
    ErrorIdentifierRejected = 2,
    ErrorServerNotAvailable = 3,
    ErrorAuthenticationFailed = 4,
    ErrorNotAuthorized = 5,

    // MQTT 5 Error Codes
    UnspecifiedError = 128,            ///< Unspecified error
    MalformedPacket = 129,             ///< Malformed packet
    ProtocolError = 130,               ///< Protocol error
    ImplementationSpecificError = 131, ///< Implementation specific error
    UnsupportedProtocolVersion = 132,  ///< Unsupported Protocol Version
    IdentifierRejected = 133,          ///< Client Identifier not valid
    AuthenticationFailed = 134,        ///< Bad username or password
    NotAuthorized = 135,               ///< Client is not authorized to connect
    ServerUnavailable = 136,           ///< The MQTT server unavailable
    ServerBusy = 137,                  ///< Server busy
    Banned = 138,                      ///< The client banned by administrative action
    BadAuthenticationMethod = 140,     ///< The authentication method is not supported
    WillTopicNameInvalid = 144,        ///< The will topic Name is not malformed but is not accepted by this Server
    PacketTooLarge = 149,              ///< The CONNECT packet exceeded the maximum permissible size
    QuotaExceeded = 151,               ///< An implementation or administrative-imposed limit has been exceeded
    PayloadFormatInvalid = 153,        ///< The will payload does not match the specified Payload Format Indicator
    RetainNotSupported = 154,          ///< The Server does not support retained messages, and Will Retain was set to 1
    WillQoSNotSupported = 155,         ///< The Server does not support the QoS set in Will QoS
    UseAnotherServer = 156,            ///< The Client should temporarily use another server
    ServerMoved = 157,                 ///< The Client should permanently use another server
    ConnectionRateExceeded = 159,      ///< The connection rate limit has been exceeded
};

XMQ_EXPORT std::string toString(ReasonCode reasonCode);

} // namespace xmq
