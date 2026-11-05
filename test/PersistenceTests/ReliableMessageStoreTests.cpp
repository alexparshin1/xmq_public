/* SPDX-License-Identifier: MPL-2.0 */
#include "server/ReliableMessageStore.h"
#include "test/PersistenceTests/PersistenceTests.h"
#include "common/RedisCredentials.h"

using namespace std;
using namespace sptk;
using namespace xmq;

class XMQ_ReliableStoreTests : public XMQ_PersistenceTests
{
public:
    void SetUp() override
    {
        XMQ_PersistenceTests::SetUp();
        stopServers();
        // These tests require an AOF-enabled Redis >=7.2. Use a private instance; no CONFIG SET
        // or other administrative changes are made to the developer's shared Redis.
        RedisConnect probe;
        probe.connect(redisConnectionUrl(TestServers::redisUri()));
        try { ReliableMessageStore::sync(probe); }
        catch (const Exception& e) { GTEST_SKIP() << "AOF/WAITAOF prerequisite: " << e.what(); }
        createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, false,
                     "primary", "user", "secret", LogPriority::Error, true, false,
                     [](Settings& settings) { settings.m_persistence.m_ack_after_store = true; });
    }

    client::SMqttClient connectClient(const string& id, bool clean = false)
    {
        auto client = make_shared<client::MqttClient>(logEngine());
        EXPECT_EQ(ReasonCode::Success,
                  client->connect(Host("localhost", TestTcpPortNumber),
                                  ConnectCredentials(id, "user", "secret"),
                                  {.m_cleanSession = clean}, ProtocolVersion::MqttV311));
        return client;
    }

    void subscribeAndDisconnect(const string& id, const string& topic)
    {
        const auto client = connectClient(id);
        Semaphore subscribed;
        client->onAck([&](const SMessage& ack)
        {
            if (ack->is(Message::Type::SubscribeAck)) subscribed.post();
        });
        client->subscribe(topic);
        ASSERT_TRUE(subscribed.wait_for(3s));
        client->disconnect();
    }

    void publishAndWait(const string& topic, const string& payload, bool retain, Qos qos = Qos::Qos1)
    {
        const auto publisher = connectClient("reliable-publisher");
        Semaphore accepted;
        publisher->onAck([&](const SMessage& ack)
        {
            if (ack->is(Message::Type::PublishAck) || ack->is(Message::Type::PublishComplete)) accepted.post();
        });
        publisher->publish(topic, payload, qos, retain);
        ASSERT_TRUE(accepted.wait_for(4s)) << "No successful publish acknowledgement";
        publisher->disconnect();
    }
};

// No sleep or wait-for-storage after ACK: the contract is that storage is already complete.
TEST_F(XMQ_ReliableStoreTests, AckImpliesOfflineDeliveryAndRetainedAlreadyStored)
{
    subscribeAndDisconnect("reliable-offline", "reliable/topic");
    publishAndWait("reliable/topic", "payload", true);
    const auto redis = server()->getRedisStorage()->getRedis();
    EXPECT_EQ(1U, redis->getHashValues("session_reliable-offline_messages").size());
    EXPECT_EQ(1U, redis->getHashValues(RetainedMessages::storageKey(server()->getNodeName())).size());
    publishAndWait("reliable/topic", "", true);
    EXPECT_TRUE(redis->getHashValues(RetainedMessages::storageKey(server()->getNodeName())).empty());
    EXPECT_EQ(2U, redis->getHashValues("session_reliable-offline_messages").size());
}

TEST_F(XMQ_ReliableStoreTests, Qos2CompletesWithStoredDelivery)
{
    subscribeAndDisconnect("reliable-qos2-offline", "reliable/qos2");
    publishAndWait("reliable/qos2", "payload", false, Qos::Qos2);
    const auto redis = server()->getRedisStorage()->getRedis();
    EXPECT_EQ(1U, redis->getHashValues("session_reliable-qos2-offline_messages").size());
    EXPECT_TRUE(redis->getHashKeys("session_reliable-publisher_qos2").empty());
}

TEST_F(XMQ_ReliableStoreTests, WrongTypeCannotLeavePartialDeliveryOrQos2Marker)
{
    const auto redis = server()->getRedisStorage()->getRedis();
    redis->setValue("reliable-wrong-type", "not-a-hash");
    ReliableMessageStore store(redisConnectionUrl(TestServers::redisUri()));
    bool installed = false;
    bool rolledBack = false;
    {
        ReliableMessageStore::Transaction transaction(store);
        transaction.hashSet("reliable-first", "id", Buffer("message"));
        transaction.hashSet("reliable-wrong-type", "id", Buffer("message"));
        transaction.hashSet("reliable-qos2-marker", "1", Buffer("1"));
        transaction.onCommit([&] { installed = true; });
        transaction.onRollback([&] { rolledBack = true; });
        EXPECT_THROW(transaction.commit(), Exception);
    }
    EXPECT_FALSE(installed);
    EXPECT_TRUE(rolledBack);
    EXPECT_TRUE(redis->getHashKeys("reliable-first").empty());
    EXPECT_TRUE(redis->getHashKeys("reliable-qos2-marker").empty());
    EXPECT_THROW((ReliableMessageStore::Transaction(store)), Exception);
}

TEST_F(XMQ_ReliableStoreTests, AbortedPublicationDoesNotBecomeVisible)
{
    ReliableMessageStore store(redisConnectionUrl(TestServers::redisUri()));
    bool installed = false;
    {
        ReliableMessageStore::Transaction transaction(store);
        transaction.hashSet("reliable-aborted", "1", Buffer("message"));
        transaction.onCommit([&] { installed = true; });
    }
    EXPECT_FALSE(installed);
    EXPECT_TRUE(server()->getRedisStorage()->getRedis()->getHashKeys("reliable-aborted").empty());
}

TEST_F(XMQ_ReliableStoreTests, BinaryPayloadAndMultipleRecipientsCommitTogether)
{
    ReliableMessageStore store(redisConnectionUrl(TestServers::redisUri()));
    const string body("zero\0byte\xff", 10);
    const Buffer value(body.data(), body.size());
    ReliableMessageStore::Transaction transaction(store);
    transaction.hashSet("reliable-a", "1", value);
    transaction.hashSet("reliable-b", "2", value);
    transaction.hashSet("reliable-marker", "3", Buffer("1"));
    transaction.commit();
    const auto redis = server()->getRedisStorage()->getRedis();
    const auto stored = redis->getHashValue("reliable-a", "1").asBuffer();
    EXPECT_EQ(body, string(stored.c_str(), stored.bytes()));
    EXPECT_FALSE(redis->getHashValue("reliable-b", "2").isNull());
    EXPECT_FALSE(redis->getHashValue("reliable-marker", "3").isNull());
}

TEST_F(XMQ_ReliableStoreTests, UnsafeSettingsAndLiveReconfigurationAreRejected)
{
    auto settings = server()->getSettings();
    CPersistence changed(settings->m_persistence);
    changed.m_enabled = false;
    EXPECT_THROW(settings->persistenceControl("set", changed), Exception);
    EXPECT_TRUE(settings->m_persistence.m_enabled.asBool());
    EXPECT_THROW(server()->getCluster()->joinCluster(Host("localhost", 1), true), Exception);
}

TEST_F(XMQ_ReliableStoreTests, InvalidStartupCannotWipeAcknowledgedRecords)
{
    subscribeAndDisconnect("reliable-protected", "reliable/protected");
    publishAndWait("reliable/protected", "keep", false);
    const auto redis = server()->getRedisStorage()->getRedis();
    auto invalid = server()->getSettings();
    const CPersistence original(invalid->m_persistence);
    const bool wasClustered = invalid->m_cluster.m_enabled.asBool();
    invalid->m_persistence.m_clean_start = true;
    EXPECT_THROW((Server(invalid, logEngine(), LogPriority::Error)), Exception);
    EXPECT_EQ(1U, redis->getHashValues("session_reliable-protected_messages").size());
    invalid->m_persistence.m_clean_start = false;
    invalid->m_persistence.m_max_queued_writes = 1;
    EXPECT_THROW((Server(invalid, logEngine(), LogPriority::Error)), Exception);
    invalid->m_persistence.m_max_queued_writes = 0;
    invalid->m_persistence.m_enabled = false;
    EXPECT_THROW((Server(invalid, logEngine(), LogPriority::Error)), Exception);
    invalid->m_persistence.m_enabled = true;
    invalid->m_cluster.m_enabled = true;
    EXPECT_THROW((Server(invalid, logEngine(), LogPriority::Error)), Exception);
    invalid->m_persistence = original;
    invalid->m_cluster.m_enabled = wasClustered;
    EXPECT_EQ(1U, redis->getHashValues("session_reliable-protected_messages").size());
}

TEST_F(XMQ_ReliableStoreTests, FullQueueRefusesPublishWithoutPartialRetainedChange)
{
    subscribeAndDisconnect("reliable-full", "reliable/full");
    server()->getClientSession("reliable-full")->setQueueLimits(1, 0);
    publishAndWait("reliable/full", "first", false);
    const auto publisher = connectClient("reliable-overflow");
    Semaphore accepted;
    publisher->onAck([&](const SMessage& ack)
    {
        if (ack->is(Message::Type::PublishReceived) || ack->is(Message::Type::PublishComplete)) accepted.post();
    });
    publisher->publish("reliable/full", "must-not-be-accepted", Qos::Qos2, true);
    EXPECT_FALSE(accepted.wait_for(500ms));
    publisher->disconnect();
    const auto redis = server()->getRedisStorage()->getRedis();
    EXPECT_EQ(1U, redis->getHashValues("session_reliable-full_messages").size());
    EXPECT_TRUE(redis->getHashValues("session_reliable-overflow_qos2").empty());
    EXPECT_TRUE(redis->getHashValues(RetainedMessages::storageKey(server()->getNodeName())).empty());
}

TEST_F(XMQ_ReliableStoreTests, NestedTransactionIsRejected)
{
    ReliableMessageStore store(redisConnectionUrl(TestServers::redisUri()));
    ReliableMessageStore::Transaction transaction(store);
    EXPECT_THROW((ReliableMessageStore::Transaction(store)), Exception);
}

TEST_F(XMQ_ReliableStoreTests, ConflictingTypesWithinBatchCannotLeavePartialState)
{
    ReliableMessageStore store(redisConnectionUrl(TestServers::redisUri()));
    ReliableMessageStore::Transaction transaction(store);
    transaction.hashSet("reliable-before-conflict", "1", Buffer("message"));
    transaction.addMember("reliable-conflict", "member");
    transaction.hashSet("reliable-conflict", "1", Buffer("message"));
    EXPECT_THROW(transaction.commit(), Exception);
    const auto redis = server()->getRedisStorage()->getRedis();
    EXPECT_TRUE(redis->getHashKeys("reliable-before-conflict").empty());
    EXPECT_TRUE(redis->getValue("reliable-conflict").isNull());
}

TEST_F(XMQ_ReliableStoreTests, ConcurrentPersistentConnectionsAndSubscriptionsRemainRecoverable)
{
    constexpr int workers = 8;
    constexpr int sessionsPerWorker = 20;
    atomic_int failures {0};
    vector<thread> threads;
    for (int worker = 0; worker < workers; ++worker)
    {
        threads.emplace_back([&, worker]
        {
            for (int index = 0; index < sessionsPerWorker; ++index)
            {
                try
                {
                    const auto id = format("reliable-concurrent-{}-{}", worker, index);
                    Semaphore subscribed;
                    const auto client = connectClient(id);
                    client->onAck([&](const SMessage& ack)
                    {
                        if (ack->is(Message::Type::SubscribeAck)) subscribed.post();
                    });
                    client->subscribe("reliable/concurrent/" + id);
                    if (!subscribed.wait_for(5s)) ++failures;
                    client->disconnect();
                }
                catch (const Exception& e)
                {
                    ++failures;
                    ADD_FAILURE() << e.what();
                }
            }
        });
    }
    for (auto& worker: threads) worker.join();
    EXPECT_EQ(0, failures.load());
    const auto redis = server()->getRedisStorage()->getRedis();
    for (int worker = 0; worker < workers; ++worker)
    {
        for (int index = 0; index < sessionsPerWorker; ++index)
        {
            const auto id = format("reliable-concurrent-{}-{}", worker, index);
            EXPECT_FALSE(redis->getValue("session_" + id).isNull()) << id;
        }
    }
}
