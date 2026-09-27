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

#include "SysTopicsTests.h"
#include "common/mqtt/PublishMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

shared_ptr<TestMqttClient> XMQ_SysTopicsTests::createTestSubscriber()
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(subscriber->isConnected());

    return subscriber;
}

tuple<shared_ptr<TestMqttClient>, shared_ptr<TestMqttClient>, std::string> XMQ_SysTopicsTests::createTestSubscriberAndPublisher()
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(subscriber->isConnected());

    auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(subscriber->isConnected());

    return tuple(subscriber, publisher, topicName);
}

uint64_t XMQ_SysTopicsTests::waitForCounter(const SystemStatistics::SysTopicKind topicKind, const uint64_t expected,
                                            const std::chrono::milliseconds timeout)
{
    const auto* statistics = server()->systemStatistics();
    const auto  deadline = std::chrono::steady_clock::now() + timeout;

    auto value = statistics->getValue(topicKind);
    while (value != expected && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        value = statistics->getValue(topicKind);
    }
    return value;
}

void XMQ_SysTopicsTests::SetUp()
{
    XMQ_ServerTests::SetUp();

    // Not an assertion: a test that starts with the broker still busy will say so through its own
    // expectations, and failing here would only hide which test left the clients behind.
    if (const auto connected = waitForCounter(SystemStatistics::SysTopicKind::BrokerClientsConnected, 0);
        connected != 0)
    {
        COUT_TS("Starting with " << connected << " client(s) still connected from an earlier test");
    }
}
