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

#include "test/SysTopicsTests/SysTopicsTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_ServerTestsLink::linkSysTopicCounterTests()
{
    // Force linking this module
}

TEST_F(XMQ_SysTopicsTests, ConnectAndDisconnect_CleanSession)
{
    using enum SystemStatistics::SysTopicKind;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto* stats = server()->systemStatistics();

    const auto initialConnectedClientCounter = stats->getValue(BrokerClientsConnected);
    const auto initialDisconnectedClientCounter = stats->getValue(BrokerClientsDisconnected);

    const TestMqttClient client(logEngine(), subscriberClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(client.isConnected());

    EXPECT_EQ(initialConnectedClientCounter + 1,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter + 1));

    client.disconnect();

    EXPECT_EQ(initialConnectedClientCounter,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter));
    // A clean session leaves nothing behind, so this one must not move at all. Waiting for a value
    // it already holds returns at once; it is written this way so the read happens after the
    // disconnect has been accounted for, not before.
    EXPECT_EQ(initialDisconnectedClientCounter,
              waitForCounter(BrokerClientsDisconnected, initialDisconnectedClientCounter));
}

TEST_F(XMQ_SysTopicsTests, ConnectAndDisconnect_PersistentSession)
{
    using enum SystemStatistics::SysTopicKind;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto* stats = server()->systemStatistics();

    const auto initialConnectedClients = stats->getValue(BrokerClientsConnected);
    const auto initialDisconnectedClients = stats->getValue(BrokerClientsDisconnected);
    const auto initialTotalClients = stats->getValue(BrokerClientsTotal);

    const TestMqttClient client(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(client.isConnected());

    EXPECT_EQ(initialConnectedClients + 1,
              waitForCounter(BrokerClientsConnected, initialConnectedClients + 1));
    // Against the total, not against the connected count it used to be compared with. The two are
    // equal only on a broker that has never had another client, which is what this test assumed.
    EXPECT_EQ(initialTotalClients + 1,
              waitForCounter(BrokerClientsTotal, initialTotalClients + 1));

    client.disconnect();

    EXPECT_EQ(initialConnectedClients, waitForCounter(BrokerClientsConnected, initialConnectedClients));
    EXPECT_EQ(initialDisconnectedClients + 1,
              waitForCounter(BrokerClientsDisconnected, initialDisconnectedClients + 1));
    EXPECT_EQ(initialTotalClients + 1, waitForCounter(BrokerClientsTotal, initialTotalClients + 1));
}

TEST_F(XMQ_SysTopicsTests, ConnectAndDisconnect_ExistingConnectedPersistentSession)
{
    using enum SystemStatistics::SysTopicKind;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto* stats = server()->systemStatistics();

    const auto initialConnectedClientCounter = stats->getValue(BrokerClientsConnected);
    const auto initialDisconnectedClientCounter = stats->getValue(BrokerClientsDisconnected);

    const TestMqttClient client1(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(client1.isConnected());

    EXPECT_EQ(initialConnectedClientCounter + 1,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter + 1));

    client1.disconnect();
    this_thread::sleep_for(100ms);

    const TestMqttClient client2(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    EXPECT_FALSE(client1.isConnected());
    EXPECT_TRUE(client2.isConnected());

    client2.disconnect();

    EXPECT_EQ(initialConnectedClientCounter,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter));
    EXPECT_EQ(initialDisconnectedClientCounter + 1,
              waitForCounter(BrokerClientsDisconnected, initialDisconnectedClientCounter + 1));
}

TEST_F(XMQ_SysTopicsTests, ConnectAndDisconnect_ExistingDisconnectedPersistentSession)
{
    using enum SystemStatistics::SysTopicKind;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const auto* stats = server()->systemStatistics();

    const auto initialConnectedClientCounter = stats->getValue(BrokerClientsConnected);
    const auto initialDisconnectedClientCounter = stats->getValue(BrokerClientsDisconnected);

    const TestMqttClient client1(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(client1.isConnected());

    EXPECT_EQ(initialConnectedClientCounter + 1,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter + 1));

    // Taking over the existing persistent connection:
    const TestMqttClient client2(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    this_thread::sleep_for(100ms);
    EXPECT_FALSE(client1.isConnected());
    EXPECT_TRUE(client2.isConnected());

    client2.disconnect();

    EXPECT_EQ(initialConnectedClientCounter,
              waitForCounter(BrokerClientsConnected, initialConnectedClientCounter));
    EXPECT_EQ(initialDisconnectedClientCounter + 1,
              waitForCounter(BrokerClientsDisconnected, initialDisconnectedClientCounter + 1));
}

TEST_F(XMQ_SysTopicsTests, ConnectAndDisconnect_MaxConnections)
{
    using enum SystemStatistics::SysTopicKind;

    const auto* stats = server()->systemStatistics();

    // The statistics used to be cleared here, which is what let the assertions below compare
    // against a bare 10. They are the broker's, not this test's: every other test in the suite
    // reads the same counters, and zeroing them while sessions from earlier tests were still
    // connected left the connected counter permanently out of step with reality - a later test
    // then saw no client where it had just connected one. That is what made this suite depend on
    // the order it ran in.
    const auto initialConnected = stats->getValue(BrokerClientsConnected);
    const auto initialMaximum = stats->getValue(BrokerClientsMaximum);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    constexpr auto                     iterations = 10U;
    vector<shared_ptr<TestMqttClient>> clients;
    for (size_t i = 0; i < iterations; ++i)
    {
        auto client = make_shared<TestMqttClient>(
            logEngine(), subscriberClientId + to_string(i),
            false, false, ProtocolVersion::MqttV31);
        EXPECT_TRUE(client->isConnected());
        clients.push_back(client);
    }

    const auto peak = initialConnected + iterations;
    EXPECT_EQ(peak, waitForCounter(BrokerClientsConnected, peak));

    // A high-water mark is not a delta - it remembers every test that came before. What this test
    // can say about it is that it is at least the number connected at once here, and never below
    // where it already stood.
    const auto maximumWhileConnected = stats->getValue(BrokerClientsMaximum);
    EXPECT_GE(maximumWhileConnected, peak);
    EXPECT_GE(maximumWhileConnected, initialMaximum);

    for (const auto& client: clients)
    {
        client->disconnect();
    }
    clients.clear();

    EXPECT_EQ(initialConnected, waitForCounter(BrokerClientsConnected, initialConnected));

    // And that it does not come down when they go.
    EXPECT_EQ(maximumWhileConnected, stats->getValue(BrokerClientsMaximum));
}
