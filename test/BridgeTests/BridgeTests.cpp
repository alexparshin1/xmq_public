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

#include "test/BridgeTests/BridgeTests.h"
#include "common/mqtt/PublishMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

const Host XMQ_BridgeTests::m_xmqServerHost("localhost", 1880);
const Host XMQ_BridgeTests::m_otherServerHost("localhost", 1886);

void XMQ_BridgeTests::SetUp()
{
    // Create two nodes: "primary" @ port 1880 and "other" @ port 1886.
    XMQ_ServerTests::SetUp();

    // Servers outlive individual tests within a suite - the base SetUp() only creates one when
    // none exists. Creating "other" unconditionally would re-bind its port for the second test.
    if (!server("other"))
    {
        // The trailing arguments are the defaults, spelled out to reach the last one: this suite is
        // what the configured bridges exist for, and every other suite now starts without them.
        createServer(m_otherServerHost.port(), TestSslPortNumber + 6, 18886,
                     true, "other", "user", "secret", sptk::LogPriority::Info, true, true);
    }
}

void XMQ_BridgeTests::waitForBridge()
{
    // Name the node explicitly: server() with no name returns the first entry of a map keyed by
    // node name, which is "other" - the node that has no bridges. Waiting on an empty bridge list
    // succeeds immediately, and the test would publish before the bridge was carrying anything.
    const auto bridgingServer = server("primary");
    ASSERT_TRUE(bridgingServer);

    const DateTime deadline = DateTime::Now() + 10s;
    while (!bridgingServer->bridgesConnected() && DateTime::Now() < deadline)
    {
        this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(bridgingServer->bridgesConnected()) << "Bridge did not connect";
}

void XMQ_ServerTestsLink::linkBridgeTests()
{
    // Force linking this module
}

tuple<client::SMqttClient, client::SMqttClient, std::string>
XMQ_BridgeTests::createTestSubscriberAndPublisher(const Host& publishToHost, const Host& subscribeToHost)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    ConnectCredentials credentials{subscriberClientId, "user", "secret"};

    auto subscriber = make_shared<client::MqttClient>(logEngine());
    auto rc = subscriber->connect(subscribeToHost, credentials, {.m_cleanSession = true}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(subscriber->isConnected());
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));

    ConnectCredentials credentials2{publisherClientId, "user", "secret"};
    auto               publisher = make_shared<client::MqttClient>(logEngine());
    rc = publisher->connect(publishToHost, credentials2, {.m_cleanSession = true}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(publisher->isConnected());

    return {subscriber, publisher, topicName};
}