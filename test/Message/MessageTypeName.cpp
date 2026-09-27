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

TEST(XMQ_Message, messageTypes)
{
    for (auto type = static_cast<int>(Message::Type::Undefined); type <= static_cast<int>(Message::Type::PingResp); ++type)
    {
        const auto messageType = static_cast<Message::Type>(type);
        EXPECT_EQ(messageType, Message::messageType(Message::messageTypeName(messageType)));
    }
}

TEST(XMQ_Message, messageTypeNames)
{
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Connect).c_str(), "Connect");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::ConnectAck).c_str(), "ConnectAck");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Publish).c_str(), "Publish");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PublishAck).c_str(), "PublishAck");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PublishReceived).c_str(), "PublishReceived");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PublishRelease).c_str(), "PublishRelease");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PublishComplete).c_str(), "PublishComplete");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Subscribe).c_str(), "Subscribe");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::SubscribeAck).c_str(), "SubscribeAck");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Unsubscribe).c_str(), "Unsubscribe");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::UnsubscribeAck).c_str(), "UnsubscribeAck");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PingReq).c_str(), "PingReq");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::PingResp).c_str(), "PingResp");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Disconnect).c_str(), "Disconnect");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Subscribe).c_str(), "Subscribe");
    EXPECT_STREQ(Message::messageTypeName(Message::Type::Undefined).c_str(), "Undefined");
}

TEST(XMQ_Message, messageTypeAndId)
{
    const AckMessage message(Message::Type::PublishAck);
    EXPECT_EQ(message.type(), Message::Type::PublishAck);

    constexpr auto testId1 = 1234;
    constexpr auto testId2 = 2345;

    AckMessage message2(Message::Type::PublishReceived, testId1);
    EXPECT_EQ(message2.type(), Message::Type::PublishReceived);
    EXPECT_EQ(testId1, message2.getId());
    EXPECT_STREQ("PublishReceived", message2.name().c_str());

    message2.setId(testId2);
    EXPECT_EQ(testId2, message2.getId());
}

TEST(XMQ_Message, properties)
{
    using enum Property;
    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(CorrelationData, "12345");

    AckMessage message(Message::Type::PublishAck, 1234);
    message.setProperties(properties);

    auto             properties2 = message.getProperties();
    std::string_view value;
    properties->getProperty(CorrelationData, value);
    std::string_view value2;
    properties->getProperty(CorrelationData, value2);
    EXPECT_EQ(value, value2);
}
