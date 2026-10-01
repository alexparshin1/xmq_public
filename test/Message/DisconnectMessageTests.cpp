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

#include "common/DisconnectMessage.h"
#include <gtest/gtest.h>

using namespace xmq;

TEST(DisconnectMessageTests, DefaultConstructor)
{
    DisconnectMessage message;

    EXPECT_EQ(message.type(), Message::Type::Disconnect);
    EXPECT_EQ(message.getReasonCode(), ReasonCode::Success);
    EXPECT_EQ(message.getQos(), Qos::Qos0);
    EXPECT_EQ(message.getId(), 0);
}

TEST(DisconnectMessageTests, ConstructorWithReasonCodeAndQos)
{
    DisconnectMessage message(ReasonCode::Success, Qos::Qos1);

    EXPECT_EQ(message.type(), Message::Type::Disconnect);
    EXPECT_EQ(message.getReasonCode(), ReasonCode::Success);
    EXPECT_EQ(message.getQos(), Qos::Qos1);
}

TEST(DisconnectMessageTests, ConstructorWithReasonCodeOnly)
{
    DisconnectMessage message(ReasonCode::UnspecifiedError);

    EXPECT_EQ(message.getReasonCode(), ReasonCode::UnspecifiedError);
    EXPECT_EQ(message.getQos(), Qos::Qos0);
}

TEST(DisconnectMessageTests, CopyConstructor)
{
    DisconnectMessage original(ReasonCode::ServerUnavailable, Qos::Qos2);
    original.setId(42);

    const DisconnectMessage copy(original);

    EXPECT_EQ(copy.type(), original.type());
    EXPECT_EQ(copy.getReasonCode(), original.getReasonCode());
    EXPECT_EQ(copy.getQos(), original.getQos());
    EXPECT_EQ(copy.getId(), original.getId());
}

TEST(DisconnectMessageTests, MoveConstructor)
{
    DisconnectMessage original(ReasonCode::ServerBusy, Qos::Qos1);
    original.setId(100);

    auto originalReasonCode = original.getReasonCode();
    auto originalQos = original.getQos();
    auto originalId = original.getId();

    DisconnectMessage moved(std::move(original));

    EXPECT_EQ(moved.getReasonCode(), originalReasonCode);
    EXPECT_EQ(moved.getQos(), originalQos);
    EXPECT_EQ(moved.getId(), originalId);
}

TEST(DisconnectMessageTests, CopyAssignment)
{
    DisconnectMessage original(ReasonCode::ProtocolError, Qos::Qos2);
    original.setId(123);

    DisconnectMessage assigned;
    assigned = original;

    EXPECT_EQ(assigned.getReasonCode(), original.getReasonCode());
    EXPECT_EQ(assigned.getQos(), original.getQos());
    EXPECT_EQ(assigned.getId(), original.getId());
}

TEST(DisconnectMessageTests, MoveAssignment)
{
    DisconnectMessage original(ReasonCode::ErrorIdentifierRejected, Qos::Qos1);
    original.setId(456);

    auto originalReasonCode = original.getReasonCode();
    auto originalQos = original.getQos();

    DisconnectMessage assigned;
    assigned = std::move(original);

    EXPECT_EQ(assigned.getReasonCode(), originalReasonCode);
    EXPECT_EQ(assigned.getQos(), originalQos);
}

TEST(DisconnectMessageTests, ToString)
{
    DisconnectMessage message(ReasonCode::Success, Qos::Qos1);
    message.setId(789);

    std::string str = message.toString();

    EXPECT_NE(str.find("Disconnect"), std::string::npos);
    EXPECT_NE(str.find("id=789"), std::string::npos);
    EXPECT_NE(str.find("qos=1"), std::string::npos);
    EXPECT_NE(str.find("reason="), std::string::npos);
}

TEST(DisconnectMessageTests, ToStringWithDefaultValues)
{
    const DisconnectMessage message;

    std::string str = message.toString();

    EXPECT_NE(str.find("Disconnect"), std::string::npos);
    EXPECT_NE(str.find("id=0"), std::string::npos);
    EXPECT_NE(str.find("qos=0"), std::string::npos);
}

TEST(DisconnectMessageTests, QosLevels)
{
    using enum Qos;
    using enum ReasonCode;

    const DisconnectMessage message0(Success, Qos0);
    EXPECT_EQ(message0.getQos(), Qos::Qos0);

    const DisconnectMessage message1(Success, Qos1);
    EXPECT_EQ(message1.getQos(), Qos::Qos1);

    const DisconnectMessage message2(Success, Qos2);
    EXPECT_EQ(message2.getQos(), Qos::Qos2);
}

TEST(DisconnectMessageTests, SetQosAfterConstruction)
{
    DisconnectMessage message(ReasonCode::Success, Qos::Qos0);
    EXPECT_EQ(message.getQos(), Qos::Qos0);

    message.setQos(Qos::Qos2);
    EXPECT_EQ(message.getQos(), Qos::Qos2);
}
