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
#include "common/ConnectMessage.h"

#include <cstdint>

namespace xmq::mqtt {

/**
 * MQTT frame types
 */
enum class FrameTypeTests : uint8_t
{
    Undefined = 0x0,
    Connect = 0x10,
    ConnAck = 0x20,
    Publish = 0x30,
    PubAck = 0x40,
    PubRec = 0x50,
    PubRel = 0x60,
    PubComp = 0x70,
    Subscribe = 0x80,
    SubAck = 0x90,
    Unsubscribe = 0xA0,
    UnsubAck = 0xB0,
    PingReq = 0xC0,
    PingResp = 0xD0,
    Disconnect = 0xE0
};

std::string frameTypeToString(const FrameTypeTests& frameType);

/**
 * MQTT connect flags for CONNECT message
 */
enum class ConnectFrameFlags : uint8_t
{
    CleanSession = 0x2,
    WillFlag = 0x4,
    WillQos1 = 0x8,
    WillQos2 = 0x10,
    WillRetain = 0x20,
    Password = 0x40,
    Username = 0x80
};

struct ConnectFlags
{
    bool    reserved : 1;
    bool    cleanSession : 1;
    bool    willFlag : 1;
    Qos     willQos : 2;
    bool    willRetain : 1;
    bool    password : 1;
    bool    username : 1;
};

constexpr FrameTypeTests FrameTypes[] {
    FrameTypeTests::Undefined,
    FrameTypeTests::ConnAck,
    FrameTypeTests::Disconnect,
    FrameTypeTests::SubAck,
    FrameTypeTests::UnsubAck,
    FrameTypeTests::PingResp,
    FrameTypeTests::PubAck,
    FrameTypeTests::Undefined,
    FrameTypeTests::Undefined,
    FrameTypeTests::Undefined,
    FrameTypeTests::PubRel,
    FrameTypeTests::PubComp,
};

constexpr FrameTypeTests messageTypeToAckFrameType(const Message::Type& messageType, const Qos qos)
{
    if (messageType == Message::Type::Publish)
    {
        return qos == Qos::Qos2 ? FrameTypeTests::PubRec : FrameTypeTests::PubAck;
    }
    if ((int) messageType > (int) Message::Type::PublishRelease)
    {
        return FrameTypeTests::Undefined;
    }
    return FrameTypes[static_cast<int>(messageType)];
}

constexpr Message::Type MessageTypes[] {
    Message::Type::Undefined,
    Message::Type::Connect,
    Message::Type::ConnectAck,
    Message::Type::Publish,
    Message::Type::PublishAck,
    Message::Type::PublishReceived,
    Message::Type::PublishRelease,
    Message::Type::PublishComplete,
    Message::Type::Subscribe,
    Message::Type::SubscribeAck,
    Message::Type::Unsubscribe,
    Message::Type::UnsubscribeAck,
    Message::Type::PingReq,
    Message::Type::PingResp,
    Message::Type::Disconnect,
};

/**
 * @brief Convert MQTT frame type to message type.
 * @param frameType         Frame type.
 * @return Messagev type.
 */
constexpr Message::Type frameTypeToMessageType(FrameTypeTests frameType)
{
    const auto frameTypeValue = static_cast<int>(frameType) >> 4;
    return MessageTypes[frameTypeValue];
}

} // namespace xmq::mqtt
