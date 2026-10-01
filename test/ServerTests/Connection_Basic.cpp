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

#include "base/MessageProperties.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * When a new connection is attempted with the client id that is already connected to the server,
 * the server has to close the existing connection in favor of the new one.
 */
TEST_F(XMQ_ServerTests, Connection_Minimal)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const TestMqttClient client(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV5);
    EXPECT_TRUE(client.isConnected());
    client.disconnect();
}

/**
 * Anonymous connections are accepted only if the server allows such connections.
 */
TEST_F(XMQ_ServerTests, Connection_AcceptAnonymous)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // Connect anonymously to a server that has anonymous connections enabled.
    server()->getSettings()->userManager().allowAnonymous(true);
    const TestMqttClient client(logEngine(), subscriberClientId, true, true, ProtocolVersion::MqttV31);
    EXPECT_TRUE(client.isConnected());
    client.disconnect();
}

/**
 * Anonymous connections are rejected if the server doesn't allow such connections.
 */
TEST_F(XMQ_ServerTests, Connection_RejectAnonymous)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // Connect anonymously to the server that has anonymous connections disabled.
    server()->getSettings()->userManager().allowAnonymous(false);
    const TestMqttClient client(logEngine(), subscriberClientId, true, true, ProtocolVersion::MqttV31);
    EXPECT_FALSE(client.isConnected());
    client.disconnect();
}

/**
 * When a new connection is attempted with the client id that is already connected to the server,
 * the server has to close the existing connection in favor of the new one.
 */
TEST_F(XMQ_ServerTests, Connection_TakeOver)
{
    Semaphore disconnected;
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    TestMqttClient client1(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV31);
    client1.onDisconnect([&disconnected](const SMessage&)
                         {
                             disconnected.post();
                         });

    const TestMqttClient client2(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV31);

    EXPECT_TRUE(disconnected.wait_for(30s));
    EXPECT_FALSE(client1.isConnected());
    EXPECT_TRUE(client2.isConnected());

    client2.disconnect();
}

/**
 * Set waiting for the first 'connect' message interval to 1 second,
 * then wait past 1 second, and expect the server to close the connection.
 */
TEST_F(XMQ_ServerTests, Connection_NoConnectMessage)
{
    const auto connectedBefore = server()->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsConnected);
    server()->setWaitForConnectMessageTimeout(1s);
    TCPSocket socket;
    socket.open(Host("localhost", TestTcpPortNumber));
    try
    {
        EXPECT_TRUE(socket.readyToRead(1500ms));
        if (socket.socketBytes() == 0)
        {
            // Server closed connection
            socket.close();
        }
    }
    catch (const Exception& e)
    {
        EXPECT_TRUE(string(e.what()).starts_with("Connection closed"));
        socket.close();
    }
    EXPECT_FALSE(socket.active());
    EXPECT_EQ(0U, socket.socketBytes());
    EXPECT_EQ(connectedBefore, server()->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsConnected));
    server()->setWaitForConnectMessageTimeout(10s);
}

/**
 * @brief Closing a TCP socket before MQTT CONNECT does not change the client count.
 */
TEST_F(XMQ_ServerTests, Connection_CloseBeforeConnectDoesNotChangeClientCount)
{
    const auto connectedBefore = server()->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsConnected);

    TCPSocket socket;
    socket.open(Host("localhost", TestTcpPortNumber));
    socket.close();

    // Let the reactor handle the peer close. An unaccepted TCP socket is not an MQTT client.
    this_thread::sleep_for(200ms);
    EXPECT_EQ(connectedBefore, server()->systemStatistics()->getValue(
        SystemStatistics::SysTopicKind::BrokerClientsConnected));
}

TEST_P(XMQ_ServerTests, Connection_InvalidProtocolVersion)
{
    auto protocolVersion = GetParam();

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");

    client::ConnectParameters connectParameters {
        .m_tweakMessage = [&protocolVersion](Buffer& message)
        {
            using enum ProtocolVersion;
            constexpr auto versionOffsetMqtt311 = 8;
            constexpr auto versionOffsetMqtt31 = 10;
            constexpr auto invalidVersionNumber = 10;
            // Set invalid protocol version
            switch (protocolVersion)
            {
                case MqttV311:
                case MqttV5:
                    message[versionOffsetMqtt311] = invalidVersionNumber;
                    break;
                case MqttV31:
                    message[versionOffsetMqtt31] = invalidVersionNumber;
                    break;
            }
        },
        .m_cleanSession = true,
    };

    auto reasonCode = client->connect(Host("localhost", TestTcpPortNumber), credentials, connectParameters, protocolVersion);
    EXPECT_FALSE(client->isConnected());
    client->disconnect();

    EXPECT_EQ(ReasonCode::ErrorInvalidProtocol, reasonCode);
}

TEST_P(XMQ_ServerTests, Connection_InvalidProtocolName)
{
    const auto protocolVersion = GetParam();

    const auto client = make_shared<client::MqttClient>(logEngine());

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");

    client::ConnectParameters connectParameters {
        .m_tweakMessage = [](Buffer& message)
        {
            // Set an invalid protocol name
            constexpr auto offset = 4;
            message[offset] = 'X';
        },
        .m_cleanSession = true,
    };

    const auto responseCode = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                              connectParameters, protocolVersion);

    this_thread::sleep_for(TinyTimeout);
    client->disconnect();

    EXPECT_EQ(protocolVersion == ProtocolVersion::MqttV5
                  ? ReasonCode::UnsupportedProtocolVersion
                  : ReasonCode::ErrorInvalidProtocol,
              responseCode);
}

TEST_P(XMQ_ServerTests, Connection_ReserveFlag)
{
    auto protocolVersion = GetParam();

    auto client = make_shared<client::MqttClient>(logEngine());

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");

    client::ConnectParameters connectParameters {
        .m_tweakMessage = [&protocolVersion](Buffer& message)
        {
            constexpr auto offset311 = 9;
            constexpr auto offset31 = 11;
            // Set invalid reserve flag
            switch (protocolVersion)
            {
                using enum ProtocolVersion;
                case MqttV311:
                case MqttV5:
                    message[offset311] |= 1;
                    break;
                case MqttV31:
                    message[offset31] |= 1;
                    break;
            }
        },
        .m_cleanSession = true,
    };

    const auto rc = client->connect(Host("localhost", TestTcpPortNumber), credentials, connectParameters, protocolVersion);

    if (protocolVersion == ProtocolVersion::MqttV5)
    {
        EXPECT_EQ(ReasonCode::MalformedPacket, rc);
    }
    else
    {
        EXPECT_EQ(ReasonCode::ErrorInvalidProtocol, rc);
    }

    client->disconnect();
}

TEST_P(XMQ_ServerTests, Connection_Accepted)
{
    const auto protocolVersion = GetParam();

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials(subscriberClientId, "user", "secret");
    const auto               responseCode = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                            {.m_tweakMessage = nullptr, .m_cleanSession = true},
                                                            protocolVersion);

    this_thread::sleep_for(10ms);

    client->disconnect();

    EXPECT_EQ(ReasonCode::Success, responseCode);
}

TEST_P(XMQ_ServerTests, Connection_AuthenticatonFailed)
{
    const auto protocolVersion = GetParam();

    const auto client = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ConnectCredentials credentials(subscriberClientId, "user", "secret2");
    const auto               responseCode = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                            {
                                                                .m_tweakMessage = nullptr,
                                                                .m_cleanSession = true,
                                              },
                                                            protocolVersion);

    this_thread::sleep_for(TinyTimeout);

    client->disconnect();

    EXPECT_EQ(ReasonCode::ErrorAuthenticationFailed, responseCode);
}

/**
 * @brief Use client id longer than 23 characters, expecting it to be rejected.
 */
TEST_P(XMQ_ServerTests, Connection_IdentifierRejected)
{
    const auto protocolVersion = GetParam();
    const auto client = make_shared<client::MqttClient>();

    const string             longClientId(260, 'X');
    const ConnectCredentials credentials(longClientId, "user", "secret");
    const auto               responseCode = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                            {
                                                                .m_tweakMessage = nullptr,
                                                                .m_cleanSession = true,
                                              },
                                                            protocolVersion);
    if (protocolVersion == ProtocolVersion::MqttV5)
    {
        EXPECT_EQ(ReasonCode::IdentifierRejected, responseCode);
    }
    else
    {
        EXPECT_EQ(ReasonCode::ErrorIdentifierRejected, responseCode);
    }

    this_thread::sleep_for(TinyTimeout);

    client->disconnect();
}
