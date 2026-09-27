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

#include "common/ConnectAckMessage.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void connectionKeepAliveTest(const ProtocolVersion protocolVersion, const bool keepAliveEnabled)
{
    Semaphore pingResponseSemaphore;

    client::MqttClient client(XMQ_ServerTests::logEngine());

    client.onAck(
        [&pingResponseSemaphore](const SMessage& message)
        {
            if (message->is(Message::Type::PingResp))
            {
                pingResponseSemaphore.post();
            }
        });

    const ConnectCredentials credentials {"test_client", "user", "secret"};
    const chrono::seconds    keepAliveInterval = keepAliveEnabled ? 1s : 0s;
    const auto               lastWill = make_shared<LastWillInfo>("last/will/topic", "last will message", false);
    const auto               rc = client.connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber), credentials,
                                                 {.m_keepAliveInterval = keepAliveInterval,
                                                  .m_lastWillInfo = lastWill,
                                                  .m_cleanSession = true},
                                                 protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    if (keepAliveEnabled)
    {
        EXPECT_TRUE(pingResponseSemaphore.wait_for(chrono::milliseconds(3000)));
    }
    else
    {
        EXPECT_FALSE(pingResponseSemaphore.wait_for(chrono::milliseconds(1100)));
        EXPECT_TRUE(client.isConnected());
    }
    client.disconnect();
}

/**
 * Check if keep alive can be disabled on the client side:
 * For a keepalive of 0 seconds, the client should not send keepalive messages
 * and receive no PingResp messages.
 */
TEST_P(XMQ_ServerTests, Connection_KeepAlive_Disabled)
{
    const ProtocolVersion protocolVersion = GetParam();
    connectionKeepAliveTest(protocolVersion, false);
}

/**
 * Check if keep alive works on the client side:
 * For a keep alive 1 second, the client should send two PingReq messages for 2 seconds
 * and receive two PingResp messages.
 */
TEST_P(XMQ_ServerTests, Connection_KeepAlive_Enabled)
{
    const ProtocolVersion protocolVersion = GetParam();
    debugLog();
    connectionKeepAliveTest(protocolVersion, true);
}

/**
 * Check if keep alive works on the client side:
 * For a keepalive 1 second, the client should send keep alive PingReq, then a message, then one more keepalive request,
 * and receive two PingResp messages.
 */
TEST_P(XMQ_ServerTests, Connection_KeepAlive_BetweenMessages)
{
    const ProtocolVersion protocolVersion = GetParam();
    Semaphore             pingResponseSemaphore;

    atomic_size_t pingResponseCount = 0;
    const auto    client = make_shared<client::MqttClient>(logEngine());

    debugLog();

    client->onAck(
        [&pingResponseCount, &pingResponseSemaphore](const SMessage& message)
        {
            if (message->is(Message::Type::PingResp))
            {
                ++pingResponseCount;
                if (constexpr auto expectedPingNumber = 2;
                    pingResponseCount == expectedPingNumber)
                {
                    pingResponseSemaphore.post();
                }
            }
        });

    const auto testNames = makeTestNames();

    const ConnectCredentials credentials {testNames.m_publisherClientId, "user", "secret"};
    const auto               lastWill = make_shared<LastWillInfo>("last/will/topic", "last will message", false);
    const auto               rc = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                  {
                                                      .m_keepAliveInterval = 1s,
                                                      .m_lastWillInfo = lastWill,
                                                      .m_cleanSession = true,
                                    },
                                                  protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    constexpr auto expirationTimeout = chrono::milliseconds(1100);

    this_thread::sleep_for(expirationTimeout);

    client->publish("test/1", "Test Message");

    this_thread::sleep_for(expirationTimeout);
    EXPECT_TRUE(pingResponseSemaphore.wait_for(chrono::milliseconds(1500)));

    client->disconnect();
}

/**
 * Check if keep alive works on the server side:
 * For a keep alive 1 second and client not sending anything for 1.5 seconds, the server should terminate the connection.
 */
TEST_P(XMQ_ServerTests, Connection_KeepAlive_Expired)
{
    const ProtocolVersion protocolVersion = GetParam();
    Semaphore             disconnectResponseSemaphore;

    const auto client = make_shared<client::MqttClient>(logEngine());

    client->onDisconnect([&disconnectResponseSemaphore](const SMessage&)
                         {
                             disconnectResponseSemaphore.post();
                         });

    const auto testNames = makeTestNames();

    const ConnectCredentials credentials {testNames.m_publisherClientId, "user", "secret"};
    const auto               lastWill = make_shared<LastWillInfo>("last/will/topic", "last will message", false);
    const auto               rc = client->connect(Host("localhost", TestTcpPortNumber), credentials,
                                                  {
                                                      .m_keepAliveInterval = 1s,
                                                      .m_lastWillInfo = lastWill,
                                                      .m_cleanSession = true,
                                    },
                                                  protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    this_thread::sleep_for(1100ms);
    client->enableKeepAlive(false);
    this_thread::sleep_for(1600ms);

    if (!disconnectResponseSemaphore.wait_for(1s) && client->isConnected())
    {
        client->disconnect();
        FAIL() << "Client was not disconnected";
    }
}
