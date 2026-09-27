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

#include "../../server/ClientSession/ClientSessionManager.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_P(XMQ_ServerTests, ClientSessionManager_ReportClients)
{
    auto protocolVersion = GetParam();

    debugLog(false);

    const auto subscriber = make_shared<client::MqttClient>();
    const auto publisher = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    ConnectCredentials credentials(subscriberClientId, "user", "secret");

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    auto rc = subscriber->connect(Host("localhost", TestTcpPortNumber), credentials,
                                  connectParameters, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    ASSERT_TRUE(subscriber->isConnected());

    ConnectCredentials credentials2(publisherClientId, "user", "secret");
    rc = publisher->connect(Host("localhost", TestTcpPortNumber), credentials2, connectParameters, protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    ASSERT_TRUE(publisher->isConnected());

    map<string, CConnectionInfo> clientSessions;
    for (const auto& clientSession: server()->getClientConnectionsInfo(RegularExpression("")))
    {
        clientSessions[clientSession.m_client_name] = clientSession;
    }
    EXPECT_TRUE(clientSessions.contains(publisherClientId));
    EXPECT_TRUE(clientSessions.contains(subscriberClientId));

    subscriber->disconnect();
    publisher->disconnect();
}
