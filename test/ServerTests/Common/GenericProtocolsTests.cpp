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

#include "base/ProtocolVersion.h"
#include "common/GenericProtocols.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

class GenericProtocolsTests : public testing::Test
{
protected:
    void SetUp() override
    {
        m_protocols = std::make_shared<GenericProtocols>(m_topicManager);
    }

    void TearDown() override
    {
        m_protocols.reset();
    }

    STopicManager     m_topicManager {std::make_shared<TopicManager>()};
    SGenericProtocols m_protocols;
};

TEST_F(GenericProtocolsTests, DefaultConstructor)
{
    ASSERT_NE(m_protocols, nullptr);
}

TEST_F(GenericProtocolsTests, AllSupportedVersionsAvailable)
{
    using enum ProtocolVersion;

    const auto& protocol31 = m_protocols->getProtocol(MqttV31);
    EXPECT_EQ(protocol31.version(), ProtocolVersion::MqttV31);

    const auto& protocol311 = m_protocols->getProtocol(MqttV311);
    EXPECT_EQ(protocol311.version(), ProtocolVersion::MqttV311);

    const auto& protocol5 = m_protocols->getProtocol(MqttV5);
    EXPECT_EQ(protocol5.version(), ProtocolVersion::MqttV5);
}

TEST_F(GenericProtocolsTests, InvalidProtocolVersion)
{
    EXPECT_THROW(m_protocols->getProtocol(static_cast<ProtocolVersion>(99)), sptk::Exception);
}

TEST_F(GenericProtocolsTests, ProtocolVersionConsistency)
{
    using enum ProtocolVersion;

    const auto& protocol1 = m_protocols->getProtocol(MqttV311);
    const auto& protocol2 = m_protocols->getProtocol(MqttV311);
    EXPECT_EQ(&protocol1, &protocol2);
}

TEST_F(GenericProtocolsTests, ProtocolHasValidComponents)
{
    const auto& protocol = m_protocols->getProtocol(ProtocolVersion::MqttV5);
    EXPECT_NE(protocol.packetReader(), nullptr);
    EXPECT_NE(protocol.messageReader(), nullptr);
    EXPECT_NE(protocol.messageWriter(), nullptr);
}
