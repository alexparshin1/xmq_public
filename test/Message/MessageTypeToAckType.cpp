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

#include <base/Message.h>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_Message, messageTypeToAckType)
{
    EXPECT_EQ(Message::Type::ConnectAck, messageTypeToAckType(Message::Type::Connect, xmq::Qos::Qos1));
    EXPECT_EQ(Message::Type::PublishAck, messageTypeToAckType(Message::Type::Publish, xmq::Qos::Qos1));
    EXPECT_EQ(Message::Type::PublishReceived, messageTypeToAckType(Message::Type::Publish, xmq::Qos::Qos2));
    EXPECT_EQ(Message::Type::PublishRelease, messageTypeToAckType(Message::Type::PublishReceived, xmq::Qos::Qos1));
    EXPECT_EQ(Message::Type::PublishComplete, messageTypeToAckType(Message::Type::PublishRelease, xmq::Qos::Qos1));
    EXPECT_EQ(Message::Type::SubscribeAck, messageTypeToAckType(Message::Type::Subscribe, Qos::Qos1));
    EXPECT_EQ(Message::Type::UnsubscribeAck, messageTypeToAckType(Message::Type::Unsubscribe, Qos::Qos1));
    EXPECT_EQ(Message::Type::Disconnect, messageTypeToAckType(Message::Type::Disconnect, Qos::Qos1));
    EXPECT_EQ(Message::Type::PingResp, messageTypeToAckType(Message::Type::PingReq, Qos::Qos1));

    EXPECT_EQ(Message::Type::Undefined, messageTypeToAckType(Message::Type::Undefined, Qos::Qos1));
}
