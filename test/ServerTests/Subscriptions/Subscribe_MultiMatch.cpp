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

#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTestsLink::linkSubscriptionTests()
{
    // Force linking this module
}

TEST_P(XMQ_ServerTests, Subscribe_MultiMatch)
{
    const auto   protocolVersion = GetParam();
    const Logger logger(*logEngine());
    const String testPayload("This is a test");

    // Connect two clients: publisher and subscriber
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto subscriber = make_shared<client::MqttClient>();
    const auto publisher = make_shared<client::MqttClient>();

    auto credentials = make_shared<ConnectCredentials>(subscriberClientId, "user", "secret");
    EXPECT_EQ(ReasonCode::Success,
              subscriber->connect(Host("localhost", TestTcpPortNumber), *credentials, {.m_cleanSession = true}, protocolVersion));

    auto credentials2 = make_shared<ConnectCredentials>(publisherClientId, "user", "secret");
    EXPECT_EQ(ReasonCode::Success,
              publisher->connect(Host("localhost", TestTcpPortNumber), *credentials2, {.m_cleanSession = true}, protocolVersion));

    Semaphore publishReceived;

    // Two subscriptions that both match what is about to be published, which is the point of this
    // test: the server must still deliver one copy.
    //
    // Built from this test's own topic rather than the literal "topic/1" and "topic/+" that were
    // here before. makeTestNames() hands out topic/1, topic/2, topic/3 by a counter, so "topic/+"
    // matched the topic of every other test in the suite - including the ones that leave a retained
    // message behind. Subscribing then delivered that retained message as well, the second wait
    // below saw it, and the test failed for a reason that had nothing to do with multiple matches.
    // It passed only because nothing had happened to run first; shuffling the order showed it.
    const auto publishTopic = topicName + "/leaf";
    const auto wildcardTopic = topicName + "/+";
    Destinations const destinations {Destination(client::MqttClient::getTopic(publishTopic)),
                                     Destination(client::MqttClient::getTopic(wildcardTopic))};
    subscriber->subscribe(destinations);

    subscriber->onMessage(
        [&publishReceived](const SPublishMessage&)
        {
            publishReceived.post();
        });

    this_thread::sleep_for(TinyTimeout);

    // Send a message with the test payload
    publisher->publish(publishTopic, testPayload);

    // Wait for the subscriber to receive that message
    EXPECT_TRUE(publishReceived.wait_for(chrono::milliseconds(100)));

    // Wait for the subscriber NOT to receive that message again
    EXPECT_FALSE(publishReceived.wait_for(chrono::milliseconds(100)));

    subscriber->disconnect();
    publisher->disconnect();
}
