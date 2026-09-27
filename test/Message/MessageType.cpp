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

#include "base/AckMessage.h"
#include "base/MessageProperties.h"
#include <base/Message.h>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_Message, messageType)
{
    EXPECT_EQ(Message::Type::Connect, Message::messageType("Connect"));
    EXPECT_EQ(Message::Type::ConnectAck, Message::messageType("ConnectAck"));
    EXPECT_EQ(Message::Type::Publish, Message::messageType("Publish"));
    EXPECT_EQ(Message::Type::PublishAck, Message::messageType("PublishAck"));
    EXPECT_EQ(Message::Type::PublishReceived, Message::messageType("PublishReceived"));
    EXPECT_EQ(Message::Type::PublishRelease, Message::messageType("PublishRelease"));
    EXPECT_EQ(Message::Type::PublishComplete, Message::messageType("PublishComplete"));
    EXPECT_EQ(Message::Type::Subscribe, Message::messageType("Subscribe"));
    EXPECT_EQ(Message::Type::SubscribeAck, Message::messageType("SubscribeAck"));
    EXPECT_EQ(Message::Type::Unsubscribe, Message::messageType("Unsubscribe"));
    EXPECT_EQ(Message::Type::UnsubscribeAck, Message::messageType("UnsubscribeAck"));
    EXPECT_EQ(Message::Type::PingReq, Message::messageType("PingReq"));
    EXPECT_EQ(Message::Type::PingResp, Message::messageType("PingResp"));
    EXPECT_EQ(Message::Type::Disconnect, Message::messageType("Disconnect"));
    EXPECT_EQ(Message::Type::Subscribe, Message::messageType("Subscribe"));
    EXPECT_EQ(Message::Type::Undefined, Message::messageType("Undefined"));
}
