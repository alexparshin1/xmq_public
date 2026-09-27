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
#include <sptk5/threads/JoiningThread.h>

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_StorageTests::sessionPersistenceTest(const bool cleanSession)
{
    const auto testStorage = make_shared<TestStorage>(server(), cleanSession);
    const auto storage = testStorage->storage();

    // const auto redis = storage->server()->getRedisStorage()->getRedis();
    //
    // // Create session record manually:
    // InsertQuery insertSessionRecord(redis, "INSERT INTO xmq_session (client_id) VALUES ('test_client_1')");
    // insertSessionRecord.exec();
    // const auto createdSessionId = insertSessionRecord.id();
    // EXPECT_NE(0, createdSessionId);
    //
    // // Create and load the session object:
    // ConnectMessageParameters connectionInfo;
    // connectionInfo.setClientId("test_client_1");
    // connectionInfo.m_protocolVersion = static_cast<uint8_t>(ProtocolVersion::MqttV31);
    // connectionInfo.m_cleanSession = cleanSession;
    //
    // const auto session = testStorage->session();
    //
    // // Create messages that belong to the session object:
    // constexpr auto maxMessageCount = 10;
    // auto           messageDeliveries = createMessageDeliveries(maxMessageCount, server().get(), testStorage.get(), 128, session);
    // this_thread::sleep_for(100ms);
    //
    // // TODO: Verify message delivery records in the database:
    // // Query countMessageDeliveryRecords(db, "SELECT COUNT(*) FROM xmq_message_delivery WHERE session_id = :session_id");
    // // countMessageDeliveryRecords.param("session_id") = session->getRecordId();
    // // auto messageDeliveryCount = countMessageDeliveryRecords.scalar().asInteger();
    // // EXPECT_EQ(maxMessageCount, messageDeliveryCount);
    //
    // storage->disconnect();
}

TEST_F(XMQ_StorageTests, cleanSession)
{
    // TODO: Re-implement session persistence/*
    //sessionPersistenceTest(true);
}

TEST_F(XMQ_StorageTests, persistentSession)
{
    // TODO: Re-implement session persistence/*
    //sessionPersistenceTest(false);
}

TEST_F(XMQ_StorageTests, loadPersistentSessionPerformance)
{
    const auto testStorage = make_shared<TestStorage>(server());

    constexpr auto maxThreads = 10;
    // 2000 rather than 10000: the loading rate is what this measures, and a fifth of the
    // records shows it just as well without spending eighty seconds of a test run.
    constexpr auto maxIterations = 2000;

    // Create session records manually:
    const auto redis = server()->getRedisStorage()->getRedis();
    redis->flush();

    // Query deleteSessionRecords(redis, "DELETE FROM xmq_session");
    // deleteSessionRecords.exec();
    //
    // Query insertSessionRecord(redis, "INSERT INTO xmq_session (client_id) VALUES (:client_id)");
    // for (auto i = 0; i < maxIterations; ++i)
    // {
    //     insertSessionRecord.param("client_id") = format("perf_test_client_{}", i);
    //     insertSessionRecord.exec();
    // }

    Stopwatch stopwatch;
    stopwatch.start();

    SynchronizedQueue<string> clientIds;
    for (auto i = 0; i < maxIterations; ++i)
    {
        clientIds.push_back(format("perf_test_client_{}", i));
    }

    JoiningThreads threads;
    for (auto i = 0; i < maxThreads; ++i)
    {
        auto thread = JoiningThread(
            [&clientIds]
            {
                const auto connectProperties = make_shared<MessageProperties>();

                const ClientSessionThreads nullThreads;
                string                     clientId;

                while (clientIds.pop_front(clientId, 1ms))
                {
                    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
                    connectMessageParameters->m_protocolVersion = ProtocolVersion::MqttV31;
                    connectMessageParameters->setClientId(clientId);
                    EXPECT_NO_THROW((void) ClientSession::factory(server().get(), connectMessageParameters, connectProperties));
                }
            });
    }

    threads.clear();

    stopwatch.stop();
    COUT("Load " << maxIterations << " sessions took " << stopwatch.seconds() * 1000 << " msec (" << maxIterations / stopwatch.milliseconds() << " K/sec)");
}

// TODO: Re-implement session messages persistence test with Redis
/*
TEST_F(XMQ_StorageTests, sessionMessagesPersistence)
{
    auto testStorage = make_shared<TestStorage>(server());

    auto storage = testStorage->storage();

    // Verify the default session exists
    auto  sqlDbConnection = dynamic_pointer_cast<SqlDbConnection>(storage->connection());
    Query selectSessionId(sqlDbConnection->databaseConnection(), "SELECT id FROM xmq_session WHERE client_id = 'client1'");
    auto  sessionId = selectSessionId.scalar().asInteger();
    EXPECT_EQ(1, sessionId);

    // Create messages in default session
    constexpr int maxMessageCount = 10;
    auto          messageDeliveries = createMessageDeliveries(maxMessageCount, server().get(), testStorage.get(), 128);
    this_thread::sleep_for(100ms);

    // Verify the messages were persisted
    Query selectMessages(sqlDbConnection->databaseConnection(), "SELECT COUNT(*) FROM xmq_message_delivery WHERE session_id = :session_id");
    selectMessages.param("session_id") = sessionId;
    auto messageCount = selectMessages.scalar().asInteger();
    EXPECT_EQ(maxMessageCount, messageCount);

    storage->disconnect();

    messageDeliveries.clear();
    testStorage->releaseDefaultSession();

    storage->connect(connectString);

    // Create the default session and load persistent messages
    testStorage->createDefaultSession(false);

    auto session = testStorage->session();
    EXPECT_EQ(sessionId, session->getRecordId());
    EXPECT_EQ(maxMessageCount, session->enqueuedMessages().size());
}
*/
