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

#include "common/GenericProtocol.h"
#include "common/MessageReaders.h"
#include "common/MessageWriters.h"
#include <gtest/gtest.h>

using namespace xmq;

class GenericProtocolTests : public testing::Test
{
protected:
    void SetUp() override
    {
        // Create mock message readers and writers for testing
        m_messageReaders = std::make_shared<MessageReaders>(m_topicManager);
        m_messageWriters = std::make_shared<MessageWriters>();
    }

    STopicManager   m_topicManager {std::make_shared<TopicManager>()};
    SMessageReaders m_messageReaders;
    SMessageWriters m_messageWriters;
};

TEST_F(GenericProtocolTests, DefaultConstructor)
{
    const GenericProtocol protocol;

    EXPECT_EQ(protocol.packetReader(), nullptr);
    EXPECT_EQ(protocol.messageReader(), nullptr);
    EXPECT_EQ(protocol.messageWriter(), nullptr);
    EXPECT_EQ(protocol.version(), ProtocolVersion::MqttV31);
}

TEST_F(GenericProtocolTests, ParameterizedConstructor)
{
    const GenericProtocol protocol(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV311);

    EXPECT_NE(protocol.packetReader(), nullptr);
    EXPECT_NE(protocol.messageReader(), nullptr);
    EXPECT_NE(protocol.messageWriter(), nullptr);
    EXPECT_EQ(protocol.version(), ProtocolVersion::MqttV311);
}

TEST_F(GenericProtocolTests, CopyConstructor)
{
    const GenericProtocol protocol1(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV5);
    const GenericProtocol protocol2(protocol1);

    EXPECT_EQ(protocol2.packetReader(), protocol1.packetReader());
    EXPECT_EQ(protocol2.messageReader(), protocol1.messageReader());
    EXPECT_EQ(protocol2.messageWriter(), protocol1.messageWriter());
    EXPECT_EQ(protocol2.version(), protocol1.version());
    EXPECT_EQ(protocol2.version(), ProtocolVersion::MqttV5);
}

TEST_F(GenericProtocolTests, MoveConstructor)
{
    GenericProtocol protocol1(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV311);
    const auto      packetReader = protocol1.packetReader();
    const auto      messageReader = protocol1.messageReader();
    const auto      messageWriter = protocol1.messageWriter();
    const auto      version = protocol1.version();

    const GenericProtocol protocol2(std::move(protocol1));

    EXPECT_EQ(protocol2.packetReader(), packetReader);
    EXPECT_EQ(protocol2.messageReader(), messageReader);
    EXPECT_EQ(protocol2.messageWriter(), messageWriter);
    EXPECT_EQ(protocol2.version(), version);
}

TEST_F(GenericProtocolTests, CopyAssignment)
{
    const GenericProtocol protocol1(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV5);
    GenericProtocol       protocol2;

    protocol2 = protocol1;

    EXPECT_EQ(protocol2.packetReader(), protocol1.packetReader());
    EXPECT_EQ(protocol2.messageReader(), protocol1.messageReader());
    EXPECT_EQ(protocol2.messageWriter(), protocol1.messageWriter());
    EXPECT_EQ(protocol2.version(), protocol1.version());
}

TEST_F(GenericProtocolTests, MoveAssignment)
{
    GenericProtocol protocol1(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV311);
    const auto      packetReader = protocol1.packetReader();
    const auto      messageReader = protocol1.messageReader();
    const auto      messageWriter = protocol1.messageWriter();
    const auto      version = protocol1.version();

    GenericProtocol protocol2;
    protocol2 = std::move(protocol1);

    EXPECT_EQ(protocol2.packetReader(), packetReader);
    EXPECT_EQ(protocol2.messageReader(), messageReader);
    EXPECT_EQ(protocol2.messageWriter(), messageWriter);
    EXPECT_EQ(protocol2.version(), version);
}

TEST_F(GenericProtocolTests, PacketReaderAccessor)
{
    const GenericProtocol protocol(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV31);

    const auto& packetReader = protocol.packetReader();
    EXPECT_NE(packetReader, nullptr);
    EXPECT_EQ(protocol.packetReader(), packetReader);
}

TEST_F(GenericProtocolTests, MessageReaderAccessor)
{
    const GenericProtocol protocol(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV31);

    const auto& messageReader = protocol.messageReader();
    EXPECT_NE(messageReader, nullptr);
    EXPECT_EQ(protocol.messageReader(), messageReader);
}

TEST_F(GenericProtocolTests, MessageWriterAccessor)
{
    const GenericProtocol protocol(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV31);

    const auto& messageWriter = protocol.messageWriter();
    EXPECT_NE(messageWriter, nullptr);
    EXPECT_EQ(protocol.messageWriter(), messageWriter);
}

TEST_F(GenericProtocolTests, VersionAccessor)
{
    const GenericProtocol protocol(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV5);

    EXPECT_EQ(protocol.version(), ProtocolVersion::MqttV5);
}

TEST_F(GenericProtocolTests, DifferentProtocolVersions)
{
    using enum ProtocolVersion;
    const GenericProtocol protocolV31(m_messageReaders, m_messageWriters, MqttV31);
    const GenericProtocol protocolV311(m_messageReaders, m_messageWriters, MqttV311);
    const GenericProtocol protocolV50(m_messageReaders, m_messageWriters, MqttV5);

    EXPECT_EQ(protocolV31.version(), MqttV31);
    EXPECT_EQ(protocolV311.version(), MqttV311);
    EXPECT_EQ(protocolV50.version(), MqttV5);

    EXPECT_NE(protocolV31.version(), protocolV311.version());
    EXPECT_NE(protocolV311.version(), protocolV50.version());
    EXPECT_NE(protocolV31.version(), protocolV50.version());
}
