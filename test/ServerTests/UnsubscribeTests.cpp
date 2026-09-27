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

#include "test/ServerTests/ServerTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
void testUnsubscribe(const ProtocolVersion protocolVersion)
{
    Semaphore publishIsReceived;

    const auto subscriber = make_shared<client::MqttClient>();

    const auto [publisherClientId, subscriberClientId, topicName] = XMQ_ServerTests::makeTestNames();

    subscriber->onMessage(
        [&publishIsReceived](const SPublishMessage&)
        {
            publishIsReceived.post();
        });

    const ConnectCredentials credentials1 {subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              subscriber->connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber), credentials1,
                                  {.m_cleanSession = false}, protocolVersion));

    // Subscriber subscribes and exits
    const SubscriptionOptions options;
    const Destination         destination(client::MqttClient::getTopic(topicName), options);
    ASSERT_TRUE(test::subscribeAndWait(subscriber, destination))
        << "The broker did not acknowledge the subscription";

    subscriber->publish(topicName, "Test Data");

    // Receive one and only one message to verify that subscribe works correctly.
    ASSERT_TRUE(publishIsReceived.wait_for(20ms));
    ASSERT_FALSE(publishIsReceived.wait_for(10ms));

    subscriber->unsubscribe({destination});
    this_thread::sleep_for(10ms);

    subscriber->publish(topicName, "Test Data");

    // Receive no messages to verify that unsubscribe works correctly.
    ASSERT_FALSE(publishIsReceived.wait_for(20ms));

    subscriber->disconnect();
}
} // namespace

TEST_P(XMQ_ServerTests, Unsubscribe_Basic)
{
    const auto protocolVersion = GetParam();
    auto       logger = debugLog(false);
    testUnsubscribe(protocolVersion);
}
