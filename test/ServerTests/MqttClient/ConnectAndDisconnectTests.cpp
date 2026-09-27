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

#include "client/MqttClient.h"
#include "client/Session.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace xmq::client;

// Test checks if the client can connect, sendDisconnect, and reconnect to the server
TEST_P(XMQ_ServerTests, MqttClient_Connect)
{
    const auto protocolVersion = GetParam();

    const Host serverHost("localhost", TestTcpPortNumber);

    auto logger = debugLog(false);

    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    MqttClient               client(logEngine());
    const ConnectCredentials credentials {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client.connect(serverHost, credentials, {.m_cleanSession = true}, protocolVersion, {}));

    ASSERT_TRUE(client.isConnected());
    client.disconnect();
}
