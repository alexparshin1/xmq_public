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

#include "common/mqtt/FrameTypeTests.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_P(XMQ_ServerTests, Connection_LastWillIfConnectionLost)
{
    const auto protocolVersion = GetParam();
    testLastWillAndTestament(protocolVersion, false);
}

TEST_P(XMQ_ServerTests, Connection_NoLastWillIfDisconnected)
{
    const auto protocolVersion = GetParam();
    testLastWillAndTestament(protocolVersion, true);
}

void XMQ_ServerTests::testLastWillAndTestament(ProtocolVersion protocolVersion, bool gracefulDisconnect)
{
    Semaphore notificationSemaphore;

    const Host serverHost("localhost", TestTcpPortNumber);

    auto logger = debugLog(false);

    // Client that is subscribed to Last Will notification
    client::MqttClient notificationClient(logEngine());

    notificationClient.onMessage(
        [&notificationSemaphore, logger](const SPublishMessage& message)
        {
            logger->debug(message->toString());
            if (string(message->payload()) == "Client lost connection")
            {
                notificationSemaphore.post();
            }
        });

    const ConnectCredentials credentials {"notification_client", "user", "secret"};
    auto                     rc = notificationClient.connect(serverHost, credentials, {.m_cleanSession = true}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    const Destination destination(client::MqttClient::getTopic("last_will/notifications"));
    notificationClient.subscribe(destination);
    this_thread::sleep_for(100ms);

    // Client that connects and terminates the connection
    client::MqttClient       client(logEngine());
    const ConnectCredentials credentials2 {"test_client", "user", "secret"};
    const auto               lastWill = make_shared<LastWillInfo>("last_will/notifications", "Client lost connection", false);
    rc = client.connect(serverHost, credentials2,
                        {
                            .m_lastWillInfo = lastWill,
                            .m_cleanSession = true,
                        },
                        protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    this_thread::sleep_for(100ms);

    if (gracefulDisconnect)
    {
        client.disconnect();
    }
    else
    {
        client.hangup();
    }

    this_thread::sleep_for(100ms);

    if (gracefulDisconnect)
    {
        // Wait for notification with last will and testament
        if (notificationSemaphore.wait_for(1s))
        {
            FAIL() << "Last Will notification arrived after client disconnected.";
        }
    }
    else
    {
        // Wait for notification with last will and testament
        if (!notificationSemaphore.wait_for(10s))
        {
            FAIL() << "Last Will notification didn't arrive after client terminated connection.";
        }
    }

    notificationClient.disconnect();
    client.disconnect();
}

TEST_P(XMQ_ServerTests, Connection_LastWillFlags)
{
    const auto protocolVersion = GetParam();
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const Host serverHost("localhost", TestTcpPortNumber);

    // Client that is subscribed to Last Will notification
    client::MqttClient client(logEngine());

    const ConnectCredentials credentials {publisherClientId, "user", "secret"};
    auto                     rc = client.connect(serverHost, credentials, {.m_cleanSession = true}, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    ASSERT_TRUE(client.isConnected());

    auto serverClient = server()->getClientSession(publisherClientId);
    if (serverClient)
    {
        EXPECT_EQ(publisherClientId, serverClient->getClientId());
        EXPECT_FALSE(serverClient->getLastWill());
    }
    else
    {
        FAIL() << "Client not found";
    }

    client.disconnect();

    this_thread::sleep_for(SmallTimeout);

    const auto lastWill = make_shared<LastWillInfo>("notifications", "Client lost connection", true, Qos::Qos1);
    rc = client.connect(serverHost, credentials,
                        {
                            .m_lastWillInfo = lastWill,
                            .m_cleanSession = true,
                        },
                        protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    ASSERT_TRUE(client.isConnected());

    serverClient = server()->getClientSession(publisherClientId);
    if (serverClient)
    {
        const auto lastWillOnServer = serverClient->getLastWill();
        ASSERT_TRUE(lastWillOnServer);
        EXPECT_TRUE(lastWillOnServer->m_retain);
        EXPECT_EQ(Qos::Qos1, lastWillOnServer->m_qos);
    }
    else
    {
        FAIL() << "Client not found";
    }

    client.disconnect();
    this_thread::sleep_for(100ms);

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;
    connectParameters.m_lastWillInfo = make_shared<LastWillInfo>("notifications", "Client lost connection", true, Qos::Qos1);
    connectParameters.m_tweakMessage =
        [&protocolVersion](Buffer& message)
    {
        // Set invalid protocol version
        constexpr auto flagsOffsetMqtt311 = 9;
        constexpr auto flagsOffsetMqtt31 = 11;
        const auto     flagsOffset = protocolVersion == ProtocolVersion::MqttV31 ? flagsOffsetMqtt31 : flagsOffsetMqtt311;
        auto*          flags = bit_cast<mqtt::ConnectFlags*>(message.data() + flagsOffset);
        flags->willQos = Qos::Invalid;
    };

    // Send invalid QOS (0x3)
    auto connectResponseCode = client.connect(serverHost, credentials, connectParameters, protocolVersion);
    EXPECT_EQ(ReasonCode::MalformedPacket, connectResponseCode);
    EXPECT_FALSE(client.isConnected());

    client.hangup();
    this_thread::sleep_for(100ms);
}
