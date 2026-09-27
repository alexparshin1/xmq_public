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
#include <gtest/gtest.h>

#include "common/mqtt/FrameTypeTests.h"

using namespace std;
using namespace xmq;
using namespace mqtt;

TEST(XMQ_Message, MessageTypeToAckFrameType)
{
    EXPECT_EQ(FrameTypeTests::ConnAck, messageTypeToAckFrameType(Message::Type::Connect, Qos::Qos1));
    EXPECT_EQ(FrameTypeTests::PubAck, messageTypeToAckFrameType(Message::Type::Publish, Qos::Qos1));
    EXPECT_EQ(FrameTypeTests::PubRec, messageTypeToAckFrameType(Message::Type::Publish, Qos::Qos2));
    EXPECT_EQ(FrameTypeTests::PubRel, messageTypeToAckFrameType(Message::Type::PublishReceived, Qos::Qos2));
    EXPECT_EQ(FrameTypeTests::PubComp, messageTypeToAckFrameType(Message::Type::PublishRelease, Qos::Qos2));
    EXPECT_EQ(FrameTypeTests::SubAck, messageTypeToAckFrameType(Message::Type::Subscribe, Qos::Qos1));
    EXPECT_EQ(FrameTypeTests::UnsubAck, messageTypeToAckFrameType(Message::Type::Unsubscribe, Qos::Qos1));
    EXPECT_EQ(FrameTypeTests::Disconnect, messageTypeToAckFrameType(Message::Type::Disconnect, Qos::Qos1));
    EXPECT_EQ(FrameTypeTests::PingResp, messageTypeToAckFrameType(Message::Type::PingReq, Qos::Qos1));
}
