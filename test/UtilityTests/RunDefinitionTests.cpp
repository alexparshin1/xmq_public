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
#include "utilities/RunDefinition.h"
#include <gtest/gtest.h>
#include <sptk5/CommandLine.h>

using namespace xmq;

class RunDefinitionTests : public testing::Test
{
protected:
    RunDefinition runDef;
};

TEST_F(RunDefinitionTests, DefaultConstructor)
{
    EXPECT_EQ(runDef.getSendCount(), 1U);
    EXPECT_EQ(runDef.getReceiveCount(), 1U);
    EXPECT_EQ(runDef.m_protocolVersion, ProtocolVersion::MqttV31);
    EXPECT_EQ(runDef.m_disconnectAfterSeconds.count(), 0);
    EXPECT_EQ(runDef.m_qos, Qos::Qos1);
    EXPECT_EQ(runDef.m_messageRate, 0U);
    EXPECT_FALSE(runDef.m_messageReadFromStdin);
    EXPECT_TRUE(runDef.m_messageWriteToStdout);
    EXPECT_EQ(runDef.m_logPriority, sptk::LogPriority::Info);
    EXPECT_EQ(runDef.m_sessionCount, 1U);
    EXPECT_EQ(runDef.m_showCounters, Reporter::CountersFormat::NoCounters);
    EXPECT_FALSE(runDef.m_sendTimestamp);
}

TEST_F(RunDefinitionTests, GetSendCount)
{
    runDef.m_sendCount = 10;
    EXPECT_EQ(runDef.getSendCount(), 10U);
}

TEST_F(RunDefinitionTests, GetReceiveCount)
{
    runDef.m_receiveCount = 20;
    EXPECT_EQ(runDef.getReceiveCount(), 20U);
}

TEST_F(RunDefinitionTests, SetTopics)
{
    runDef.m_topics.push_back("topic1");
    runDef.m_topics.push_back("topic2");

    EXPECT_EQ(runDef.m_topics.size(), 2U);
    EXPECT_EQ(runDef.m_topics[0], "topic1");
    EXPECT_EQ(runDef.m_topics[1], "topic2");
}

TEST_F(RunDefinitionTests, SetMessage)
{
    const char* testMessage = "Test message content";
    runDef.m_message.set(testMessage, strlen(testMessage));

    EXPECT_EQ(runDef.m_message.size(), strlen(testMessage));
    EXPECT_STREQ(reinterpret_cast<const char*>(runDef.m_message.data()), testMessage);
}

TEST_F(RunDefinitionTests, SetBindAddresses)
{
    runDef.m_bindAddresses.push_back("127.0.0.1");
    runDef.m_bindAddresses.push_back("192.168.1.1");

    EXPECT_EQ(runDef.m_bindAddresses.size(), 2U);
    EXPECT_EQ(runDef.m_bindAddresses[0], "127.0.0.1");
    EXPECT_EQ(runDef.m_bindAddresses[1], "192.168.1.1");
}

TEST_F(RunDefinitionTests, CredentialsAccess)
{
    EXPECT_NO_THROW({
        auto& creds = runDef.m_credentials;
        (void) creds;
    });
}

TEST_F(RunDefinitionTests, ConnectParametersAccess)
{
    EXPECT_NO_THROW({
        auto& params = runDef.m_connectParameters;
        (void) params;
    });
}

TEST_F(RunDefinitionTests, ConnectPropertiesAccess)
{
    EXPECT_NO_THROW({
        auto& props = runDef.m_connectProperties;
        (void) props;
    });
}

TEST_F(RunDefinitionTests, ServerHostAccess)
{
    EXPECT_NO_THROW({
        auto& host = runDef.m_serverHost;
        (void) host;
    });
}

TEST_F(RunDefinitionTests, SSLKeysAccess)
{
    EXPECT_EQ(runDef.m_sslKeys, nullptr);

    runDef.m_sslKeys = std::make_shared<sptk::SSLKeys>();
    EXPECT_NE(runDef.m_sslKeys, nullptr);
}
