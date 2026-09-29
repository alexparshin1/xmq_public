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
#include "server/MessageDelivery.h"
#include "test/PersistenceTests/PersistenceTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

shared_ptr<client::MqttClient> connectClient(const string& clientId, const bool cleanSession,
                                             const PublishMessageCallback& messageCallback = nullptr)
{
    auto client = make_shared<client::MqttClient>(XMQ_PersistenceTests::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = cleanSession;

    if (messageCallback)
    {
        client->onMessage(messageCallback);
    }

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", XMQ_PersistenceTests::TestTcpPortNumber),
                              ConnectCredentials(clientId, "user", "secret"),
                              connectParameters, ProtocolVersion::MqttV311));

    return client;
}

size_t sessionRecordCount(const string& clientId)
{
    const auto redis = XMQ_PersistenceTests::server()->getRedisStorage()->getRedis();
    return redis->getHashValues("session_" + clientId + "_messages").size();
}

} // namespace

// With max_queued_writes at 1 nearly every PUBLISH arrives while a record write is still
// unconfirmed, so the publisher's session is paused and resumed over and over. Each pause leaves
// the PUBLISH it stopped at in the read buffer; nothing may be lost or reordered by that, and the
// subscriber - whose session is never paused - keeps receiving throughout.
TEST_F(XMQ_PersistenceTests, WriteCapacity_PausedPublisherLosesNothing)
{
    const auto& settings = server()->getSettings();
    settings->m_persistence.m_max_queued_writes = 1;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    constexpr int messageCount = 500;
    atomic_int    received {0};
    atomic_bool   ordered {true};
    Semaphore     allReceived;

    // A persistent session, so every delivery to it writes a record.
    const auto subscriber = connectClient(subscriberClientId, false,
                                          [&](const SPublishMessage& message)
                                          {
                                              const string payload(bit_cast<const char*>(message->payloadData()),
                                                                   message->payloadSize());
                                              if (payload != to_string(received.load()))
                                              {
                                                  ordered = false;
                                              }
                                              if (++received == messageCount)
                                              {
                                                  allReceived.post();
                                              }
                                          });
    subscriber->subscribe(topicName);

    const auto pausesBefore = MessageDelivery::writeCapacityPauses();

    const auto publisher = connectClient(publisherClientId, true);
    for (int i = 0; i < messageCount; ++i)
    {
        publisher->publish(topicName, to_string(i), Qos::Qos1);
    }

    EXPECT_TRUE(allReceived.wait_for(20s)) << "Received " << received.load() << " of " << messageCount;
    EXPECT_TRUE(ordered.load()) << "Messages arrived out of order";
    EXPECT_GT(MessageDelivery::writeCapacityPauses(), pausesBefore) << "The publisher was never paused, so the test proved nothing";

    // Every delivery was acknowledged, so whatever was written has been deleted again and whatever
    // was not written yet was dropped: nothing may be left behind.
    this_thread::sleep_for(200ms);
    EXPECT_EQ(0U, sessionRecordCount(subscriberClientId));

    publisher->disconnect();
    subscriber->disconnect();
    settings->m_persistence.m_max_queued_writes.setNull();
}

// Write-behind drops a record only when its delivery finished first. A subscriber that is not
// connected finishes nothing, so every record must reach Redis - and be gone again once the
// subscriber has come back and acknowledged them.
TEST_F(XMQ_PersistenceTests, WriteBehind_OfflineSubscriberRecordsAreWritten)
{
    const auto& settings = server()->getSettings();
    settings->m_persistence.m_max_queued_writes = 1000;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    constexpr int messageCount = 20;

    {
        const auto subscriber = connectClient(subscriberClientId, false);
        subscriber->subscribe(topicName);
        this_thread::sleep_for(100ms);
        subscriber->disconnect();
    }

    const auto publisher = connectClient(publisherClientId, true);
    for (int i = 0; i < messageCount; ++i)
    {
        publisher->publish(topicName, to_string(i), Qos::Qos1);
    }
    this_thread::sleep_for(200ms);
    EXPECT_EQ(static_cast<size_t>(messageCount), sessionRecordCount(subscriberClientId))
        << "Records for an offline subscriber were not written";

    atomic_int received {0};
    Semaphore  allReceived;
    const auto subscriber = connectClient(subscriberClientId, false,
                                          [&](const SPublishMessage&)
                                          {
                                              if (++received == messageCount)
                                              {
                                                  allReceived.post();
                                              }
                                          });
    EXPECT_TRUE(allReceived.wait_for(5s)) << "Received " << received.load() << " of " << messageCount;
    this_thread::sleep_for(200ms);
    EXPECT_EQ(0U, sessionRecordCount(subscriberClientId)) << "Acknowledged records were not deleted";

    publisher->disconnect();
    subscriber->disconnect();
    settings->m_persistence.m_max_queued_writes.setNull();
}
