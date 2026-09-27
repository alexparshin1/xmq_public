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

#include "StorageTests.h"
#include "TestStorage.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace xmq {

TEST_F(XMQ_StorageTests, subscriptionCleanSession)
{
    testPersistentSubscription(true);
}

TEST_F(XMQ_StorageTests, subscriptionContinueSession)
{
    testPersistentSubscription(false);
}

TEST_F(XMQ_StorageTests, subscriptionContinueSessionPerformance)
{
    testPersistentSubscriptionPerformance(false);
}

void XMQ_StorageTests::testPersistentSubscription(const bool cleanSession)
{
    const ClientSessionThreads nullThreads;

    const auto  testStorage = make_shared<TestStorage>(server(), cleanSession);
    //const auto& storage = testStorage->storage();

    // const auto db = storage->getDatabaseConnection();
    //
    // Query deleteAllSessions(db, "DELETE FROM xmq_session");
    // deleteAllSessions.exec();
    //
    // // Create session record manually:
    // InsertQuery insertSessionRecord(db, "INSERT INTO xmq_session (client_id) VALUES ('test_client_1')");
    // insertSessionRecord.exec();
    // const auto createdSessionId = static_cast<RecordId>(insertSessionRecord.id());
    // EXPECT_NE(0, createdSessionId);
    //
    // // Create and load the session object:
    // const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    // connectMessageParameters->setClientId("test_client_1");
    // connectMessageParameters->m_protocolVersion = static_cast<uint8_t>(ProtocolVersion::MqttV31);
    // connectMessageParameters->m_cleanSession = cleanSession;
    //
    // const auto session = ClientSession::factory(server().get(), connectMessageParameters);
    // // TODO: FIXME
    // // EXPECT_EQ(createdSessionId, session->getRecordId());
    //
    // auto        seconds = chrono::duration_cast<chrono::seconds>(DateTime::Now().sinceEpoch()).count();
    // const auto* topic = server()->getTopicManager()->getTopic(format("device/{}", seconds));
    // const auto  subscription = server()->getSubscriptionManager()->subscribe(topic, session, Qos::Qos1, 123, SubscriptionOptions());
    //
    // this_thread::sleep_for(200ms);
    //
    // Query getSubscriptionRecord(db,
    //                             format("SELECT * FROM xmq_session_subscription WHERE session_id = {}", createdSessionId));
    // getSubscriptionRecord.open();
    //
    // auto referencedSessionId = getSubscriptionRecord["session_id"].asInteger();
    // EXPECT_EQ(referencedSessionId, createdSessionId);
    //
    // auto referencedSubscriptionId = getSubscriptionRecord["topic"].asString();
    // EXPECT_EQ(referencedSubscriptionId, subscription->fullName());
    //
    // getSubscriptionRecord.close();
}

void XMQ_StorageTests::testPersistentSubscriptionPerformance(const bool cleanSession)
{
    // const ClientSessionThreads nullThreads;
    //
    // const auto storage = server()->getStorage();
    //
    // const auto db = storage->getDatabaseConnection();
    //
    // const auto subscriptionManager = server()->getSubscriptionManager();
    // subscriptionManager->clear();
    // this_thread::sleep_for(100ms);
    //
    // Query deleteAllSessions(db, "DELETE FROM xmq_session");
    // deleteAllSessions.exec();
    //
    // Query deleteAllSubscriptions(db, "DELETE FROM xmq_subscription");
    // deleteAllSubscriptions.exec();
    //
    // // Create session record manually:
    // InsertQuery insertSessionRecord(db, "INSERT INTO xmq_session (client_id) VALUES ('test_client_1')");
    // insertSessionRecord.exec();
    // const auto createdSessionId = static_cast<RecordId>(insertSessionRecord.id());
    // EXPECT_NE(0, createdSessionId);
    //
    // // Create and load the session object:
    // auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    // connectMessageParameters->setClientId("test_client_1");
    // connectMessageParameters->m_protocolVersion = static_cast<uint8_t>(ProtocolVersion::MqttV31);
    // connectMessageParameters->m_cleanSession = cleanSession;
    //
    // const auto session = ClientSession::factory(server().get(), connectMessageParameters);
    // // TODO: FIXME
    // // EXPECT_EQ(createdSessionId, session->getRecordId());
    //
    // Stopwatch stopwatch;
    //
    // stopwatch.start();
    // constexpr size_t maxSubscriptions = 1000;
    // for (size_t i = 0; i < maxSubscriptions; ++i)
    // {
    //     const auto* topic = server()->getTopicManager()->getTopic(format("device/{}", i));
    //     server()->getSubscriptionManager()->subscribe(topic, session, Qos::Qos1, 123, SubscriptionOptions());
    // }
    // stopwatch.stop();
    // COUT_TS(format("Subscribed {} times in {:0.1f} ms: {:0.1f}K/sec", maxSubscriptions, stopwatch.milliseconds(), maxSubscriptions / stopwatch.milliseconds()));
    //
    // this_thread::sleep_for(3000ms);
    //
    // Query getSubscriptionRecordCount(db, "SELECT COUNT(*) FROM xmq_subscription WHERE topic LIKE 'device%'");
    // auto  recordCount = getSubscriptionRecordCount.scalar();
    // EXPECT_EQ(maxSubscriptions, recordCount.asInteger());
    //
    // Query getSessionSubscriptionRecordCount(db, "SELECT COUNT(*) FROM xmq_session_subscription");
    // recordCount = getSessionSubscriptionRecordCount.scalar();
    // EXPECT_EQ(maxSubscriptions, recordCount.asInteger());
    //
    // for (size_t i = 0; i < maxSubscriptions; ++i)
    // {
    //     const auto* topic = server()->getTopicManager()->getTopic(format("device/{}", i));
    //     server()->getSubscriptionManager()->unsubscribe(topic, session.get());
    // }
}

} // namespace xmq
