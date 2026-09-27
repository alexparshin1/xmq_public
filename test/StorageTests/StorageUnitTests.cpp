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
#include "test/StorageTests/TestStorage.h"

#include <ranges>
#include <sptk5/Printer.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr auto testReceiveMaximum = 1000;
constexpr auto testSubscriptionId = 98765;

void createTopicNames(size_t maxSubscriptions, Strings& topicNames, Strings& topicWildCards, size_t officeOffset)
{
    const Strings systemNames {format("office{}", officeOffset),
                               format("office{}", officeOffset + 1),
                               "hr1", "hr2", "user1", "user2", "user3"};
    const Strings deviceTypes {"usb", "usb2", "usb3", "ide", "scsi", "scsi2", "scsi3", "serial", "lpt", "vga",
                               "super_vga", "dvi"};

    topicNames.clear();
    topicWildCards.clear();
    for (const auto& systemName: systemNames)
    {
        // Create topic names
        for (const auto& deviceType: deviceTypes)
        {
            for (size_t index = 0; index < maxSubscriptions; ++index)
            {
                stringstream topicName;
                topicName << systemName << "/" << deviceType << "/" << index;
                topicNames.emplace_back(topicName.str());
            }
        }

        // Create wildcards
        for (size_t index = 0; index < maxSubscriptions; ++index)
        {
            stringstream wildcard;
            wildcard << systemName << "/+/" << index;
            topicWildCards.emplace_back(wildcard.str());
        }
    }
}

void subscribeClient(TopicManager& topicManager, SubscriptionManager& subscriptionManager, const SClientSession& connection,
                     const Strings& topicNames)
{
    for (const auto& topicName: topicNames)
    {
        const auto* topic = topicManager.getTopic(topicName);
        subscriptionManager.subscribe(topic, connection, Qos::Qos0, 0, SubscriptionOptions());
    }
}

} // namespace

/**
 * Test creating a new persistent subscription from a session.
 */
namespace {

/**
 * @brief Waits for a subscription count to reach what is expected of it, and returns what it saw.
 *
 * Subscribing writes to storage without waiting, so these tests used fixed sleeps - a second here,
 * half a second there - which are guesses about how busy the machine is. Shuffling the suite made
 * the guess wrong: persistentSubscriptionReconnect looked for its subscription after a second and
 * found nothing, because the work queued ahead of it depended on what had run before.
 *
 * Returns the last value read either way, so a real failure still reports the count rather than a
 * timeout.
 */
size_t waitForSubscriptionCount(const TestStorage& storage, const String& topicName,
                                const size_t                    expected,
                                const std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    auto count = storage.countSessionSubscriptions(topicName);
    while (count != expected && std::chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(20ms);
        count = storage.countSessionSubscriptions(topicName);
    }
    return count;
}

} // namespace

TEST_F(XMQ_StorageTests, persistentSubscriptionCreate)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);

    COUT("Redis: " << testStorage->storage()->toString());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    auto testDeviceId = DateTime::Now().sinceEpoch().count();

    const auto topicName1 = format("device/id/{}", testDeviceId);
    testDeviceId++;
    const auto topicName2 = format("$share/group1/device/id/{}", testDeviceId);

    const auto* topic1 = topicManager->getTopic(topicName1);
    subscriptionManager->subscribe(topic1, testStorage->session(), Qos::Qos1, testSubscriptionId, SubscriptionOptions());

    const auto* topic2 = topicManager->getTopic(topicName2);
    subscriptionManager->subscribe(topic2, testStorage->session(), Qos::Qos1, testSubscriptionId, SubscriptionOptions());

    EXPECT_EQ(1U, waitForSubscriptionCount(*testStorage, topicName1, 1));
    EXPECT_EQ(1U, waitForSubscriptionCount(*testStorage, topicName2, 1));

    testStorage->releaseDefaultSession();
}

namespace {
tuple<string, string> createTestTopicsNames()
{
    auto       deviceId = DateTime::Now().sinceEpoch().count();
    const auto topicName1 = format("device/id/{}", deviceId);
    deviceId++;
    const auto topicName2 = format("device/id/{}", deviceId);
    return {topicName1, topicName2};
}
} // namespace


// Create a new persistent connection and verify it's there after closing and re-opening Storage
TEST_F(XMQ_StorageTests, persistentSubscriptionReconnect)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto [topicName1, topicName2] = createTestTopicsNames();

    subscriptionManager->subscribe(topicManager->getTopic(topicName1), testStorage->session(), Qos::Qos1, 0, SubscriptionOptions());
    subscriptionManager->subscribe(topicManager->getTopic(topicName2), testStorage->session(), Qos::Qos1, 0, SubscriptionOptions());

    EXPECT_EQ(1U, waitForSubscriptionCount(*testStorage, topicName1, 1));
    EXPECT_EQ(1U, waitForSubscriptionCount(*testStorage, topicName2, 1));

    subscriptionManager->unsubscribe(topicManager->getTopic(topicName1), testStorage->session().get());
    subscriptionManager->unsubscribe(topicManager->getTopic(topicName2), testStorage->session().get());

    EXPECT_EQ(0U, waitForSubscriptionCount(*testStorage, topicName1, 0));
    EXPECT_EQ(0U, waitForSubscriptionCount(*testStorage, topicName2, 0));

    testStorage->releaseDefaultSession();
}

// Create a new persistent connection, then delete it and verify it isn't there
TEST_F(XMQ_StorageTests, persistentSubscriptionDelete)
{
    const TestStorage testStorage(server(), false);

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto [topicName1, topicName2] = createTestTopicsNames();

    subscriptionManager->subscribe(topicManager->getTopic(topicName1), testStorage.session(), Qos::Qos1, 0, SubscriptionOptions());
    EXPECT_EQ(1U, waitForSubscriptionCount(testStorage, topicName1, 1));

    subscriptionManager->unsubscribe(topicManager->getTopic(topicName1), testStorage.session().get());
    EXPECT_EQ(0U, waitForSubscriptionCount(testStorage, topicName1, 0));
}

// Create a new persistent connection, then delete it and verify it isn't there

namespace {

size_t officeOffset = 1;

void testSubscribePerformance(const SServer& server)
{
    const auto        topicManager = server->getTopicManager();
    const TestStorage storage(server);

    const auto subscriptionManager = server->getSubscriptionManager();

    Stopwatch stopwatchTotal;
    stopwatchTotal.start();
    try
    {
        constexpr auto maxSubscriptions = 256;

        Strings topicNames;
        Strings topicWildcards;
        officeOffset += 2;
        createTopicNames(maxSubscriptions, topicNames, topicWildcards, officeOffset);
        subscribeClient(*topicManager, *subscriptionManager, storage.session(), topicNames);

        Stopwatch stopwatch;
        stopwatch.start();
        for (const auto& topicName: topicNames)
        {
            subscriptionManager->subscribe(topicManager->getTopic(topicName), storage.session(), Qos::Qos0, 0, SubscriptionOptions());
        }
        stopwatch.stop();

        COUT("findTopicPerformance: ");
        COUT("Subscribed " << topicNames.size() << " for " << setprecision(2) << stopwatch.seconds() << " sec: "
                           << fixed << setprecision(1) << static_cast<double>(topicNames.size()) / stopwatch.seconds() / 1000 << "K per sec");

        stopwatch.start();
        for (const auto& topicName: topicNames)
        {
            const auto topic = topicManager->getTopic(topicName);
            subscriptionManager->find(topic,
                                      [](const SubscriptionGroup&)
                                      {
                                          return Subscriptions::ActionType::Continue;
                                      });
        }
        stopwatch.stop();

        COUT("Found " << topicNames.size() << " for " << setprecision(2) << stopwatch.seconds() << " sec: "
                      << fixed << setprecision(1) << static_cast<double>(topicNames.size()) / stopwatch.seconds() / 1000 << "K per sec");
    }
    catch (const Exception& e)
    {
        CERR(e.message());
    }
    stopwatchTotal.stop();
    const String remark = ", including database flush time,";
    COUT("Total test time" << remark << " was " << fixed << setprecision(1) << stopwatchTotal.seconds() << " sec");
    this_thread::sleep_for(500ms);
}

void verifyIfSubscribed(const SClientSession& clientSession, const Strings& topicNames, const bool expectedResult)
{
    for (const auto& topicName: topicNames)
    {
        EXPECT_EQ(expectedResult, clientSession->isSubscribed(topicName));
    }
}

} // namespace

TEST_F(XMQ_StorageTests, persistentSubscriptionPerformance)
{
    constexpr auto iterations = 3000;

    TestStorage testStorage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    connectMessageParameters->setClientId(format("client-{}", DateTime::Now().sinceEpoch().count()));
    connectMessageParameters->m_protocolVersion = ProtocolVersion::MqttV31;

    const auto connectProperties = make_shared<MessageProperties>();
    connectProperties->setProperty(Property::ReceiveMaximum, testReceiveMaximum);
    connectProperties->setProperty(Property::AuthenticationMethod, "plain");

    const auto clientSession = ClientSession::factory(server().get(), connectMessageParameters, connectProperties);
    server()->getClientSessionManager()->add(clientSession);

    Stopwatch stopwatch;
    stopwatch.start();
    for (size_t i = 0; i < iterations; ++i)
    {
        subscriptionManager->subscribe(topicManager->getTopic(format("device/usb/{}", i)), clientSession, Qos::Qos1, 0, SubscriptionOptions());
    }
    stopwatch.stop();
    COUT(format("Subscribed   {} times in {:0.2f} seconds: {:0.1f}K/sec",
                iterations, stopwatch.seconds(), static_cast<double>(iterations) / stopwatch.milliseconds()));

    stopwatch.start();
    for (size_t i = 0; i < iterations; ++i)
    {
        subscriptionManager->unsubscribe(topicManager->getTopic(format("device/usb/{}", i)), clientSession.get());
    }
    stopwatch.stop();
    COUT(format("Unsubscribed {} times in {:0.2f} seconds: {:0.1f}K/sec",
                iterations, stopwatch.seconds(), static_cast<double>(iterations) / stopwatch.milliseconds()));
}

// Find matches of topic subscriptions to a topic
TEST_F(XMQ_StorageTests, subscribePerformance_Storage)
{
    testSubscribePerformance(server());
}

// Find matches of topic subscriptions to a topic
TEST_F(XMQ_StorageTests, subscribePerformance_memory)
{
    testSubscribePerformance(server());
}

/**
 * Find matches of wildcard subscriptions to a topic
 */
TEST_F(XMQ_StorageTests, findTopicInWildcardsPerformance)
{
    TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    constexpr auto maxSubscriptions = 10;

    Strings topicNames;
    Strings topicWildcards;
    officeOffset += 2;
    createTopicNames(maxSubscriptions, topicNames, topicWildcards, officeOffset);
    subscribeClient(*topicManager, *subscriptionManager, storage.session(), topicWildcards);

    Stopwatch stopwatch;
    stopwatch.start();
    for (const auto& topicName: topicNames)
    {
        const auto* topic = topicManager->getTopic(topicName);
        subscriptionManager->subscribe(topic, storage.session(), Qos::Qos0, 0, SubscriptionOptions());
    }
    stopwatch.stop();

    COUT("findTopicInWildcardsPerformance: ");
    COUT("Subscribed " << topicNames.size() << " for " << setprecision(2) << stopwatch.seconds() << " sec: "
                       << fixed << setprecision(1) << static_cast<double>(topicNames.size()) / stopwatch.seconds() / 1000 << "K per sec");

    stopwatch.start();
    for (const auto& topicName: topicNames)
    {
        const auto topic = topicManager->getTopic(topicName);
        subscriptionManager->find(topic,
                                  [](const SubscriptionGroup&)
                                  {
                                      return Subscriptions::ActionType::Continue;
                                  });
    }
    stopwatch.stop();

    COUT("Found " << topicNames.size() << " for " << setprecision(2) << stopwatch.seconds() << " sec: "
                  << fixed << setprecision(1) << static_cast<double>(topicNames.size()) / stopwatch.seconds() / 1000 << "K per sec");
}

TEST_F(XMQ_StorageTests, findWildcard)
{
    TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    constexpr auto maxSubscriptions = 2;

    ConnectMessageParameters connectionInfo;
    connectionInfo.setClientId("client11");

    const auto connectProperties = make_shared<MessageProperties>();
    connectProperties->setProperty(Property::ReceiveMaximum, testReceiveMaximum);
    connectProperties->setProperty(Property::AuthenticationMethod, "plain");

    const auto connection = storage.session();

    Strings topicNames;
    Strings topicWildcards;
    officeOffset += 2;
    createTopicNames(maxSubscriptions, topicNames, topicWildcards, officeOffset);

    subscribeClient(*topicManager, *subscriptionManager, connection, topicNames);
    this_thread::sleep_for(500ms);

    COUT("Subscribed " << topicNames.size() << " topics");

    const auto wildcard1 = topicManager->getTopic(format("office{}/+/1", officeOffset + 1));
    size_t     subscriptionCounter = 0;

    subscriptionManager->find(wildcard1,
                              [&subscriptionCounter](const SubscriptionGroup&)
                              {
                                  ++subscriptionCounter;
                                  return Subscriptions::ActionType::Continue;
                              });
    EXPECT_EQ(12U, subscriptionCounter);

    const auto wildcard2 = topicManager->getTopic(format("office{}/#", officeOffset + 1));
    subscriptionCounter = 0;
    subscriptionManager->find(wildcard2,
                              [&subscriptionCounter](const SubscriptionGroup&)
                              {
                                  ++subscriptionCounter;
                                  return Subscriptions::ActionType::Continue;
                              });
    EXPECT_EQ(24U, subscriptionCounter);

    this_thread::sleep_for(100ms);

    storage.releaseDefaultSession();
}

/**
 * Check if a topic can be matched to existing wildcard subscriptions
 */
TEST_F(XMQ_StorageTests, findWildcards)
{
    TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto connection = storage.session();

    const Strings wildcards({"office1/+/1", "office2/+/1"});
    subscribeClient(*topicManager, *subscriptionManager, connection, wildcards);

    const auto topic = topicManager->getTopic("office2/usb/1");
    size_t     subscriptionCounter = 0;

    subscriptionManager->find(topic,
                              [&subscriptionCounter](const SubscriptionGroup& subscriptionGroup)
                              {
                                  for (const auto& subscription: subscriptionGroup | views::values)
                                  {
                                      subscriptionCounter += subscription->clientCount();
                                  }
                                  return Subscriptions::ActionType::Continue;
                              });
    EXPECT_EQ(1U, subscriptionCounter);

    const Strings wildcards2({"office2/#"});
    subscribeClient(*topicManager, *subscriptionManager, connection, wildcards2);

    subscriptionCounter = 0;

    subscriptionManager->find(topic,
                              [&subscriptionCounter](const SubscriptionGroup& subscriptionGroup)
                              {
                                  for (const auto& subscription: subscriptionGroup | views::values)
                                  {
                                      subscriptionCounter += subscription->clientCount();
                                  }
                                  return Subscriptions::ActionType::Continue;
                              });
    EXPECT_EQ(2U, subscriptionCounter);

    this_thread::sleep_for(100ms);

    storage.releaseDefaultSession();
}

TEST_F(XMQ_StorageTests, subcribeSameWildcards_not_persistent)
{
    const TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    connectMessageParameters->setClientId("client1");

    const auto connectProperties = make_shared<MessageProperties>();
    connectProperties->setProperty(Property::ReceiveMaximum, testReceiveMaximum);
    connectProperties->setProperty(Property::AuthenticationMethod, "plain");

    const auto connection = ClientSession::factory(server().get(), connectMessageParameters, connectProperties);

    const Strings wildcards({"office1/+/1", "office1/+/1"});
    subscribeClient(*topicManager, *subscriptionManager, connection, wildcards);
}

TEST_F(XMQ_StorageTests, subcribeSameWildcards_persistent)
{
    const TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    auto connection = server()->getClientSessionManager()->find("client1");
    if (!connection)
    {
        const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
        connectMessageParameters->setClientId("client1");

        const auto connectProperties = make_shared<MessageProperties>();
        connectProperties->setProperty(Property::ReceiveMaximum, testReceiveMaximum);
        connectProperties->setProperty(Property::AuthenticationMethod, "plain");

        connection = ClientSession::factory(server().get(), connectMessageParameters, connectProperties);
        server()->getClientSessionManager()->add(connection);
    }

    const Strings wildcards({"office1/+/1", "office1/+/1"});
    subscribeClient(*topicManager, *subscriptionManager, connection, wildcards);
}

namespace {

void verifySubscribedTopics(SubscriptionManager& subscriptionManager, const STopicManager& topicManager, const string_view topicName, const size_t expectedCount)
{
    size_t count = 0;
    subscriptionManager.find(topicManager->getTopic(topicName),
                             [&count](const auto& subscriptionGroup)
                             {
                                 for (const auto& subscription: views::values(subscriptionGroup))
                                 {
                                     count += subscription->clientCount();
                                 }
                                 return Subscriptions::ActionType::Continue;
                             });
    EXPECT_EQ(expectedCount, count);
}

void unsubscribeTests(const SServer& server, const Strings& topicNames)
{
    const TestStorage storage(server);

    const auto topicManager = server->getTopicManager();
    const auto subscriptionManager = server->getSubscriptionManager();

    const auto clientSession = storage.session();

    EXPECT_FALSE(clientSession->isSubscribed(topicNames[0]));
    EXPECT_FALSE(clientSession->isSubscribed(topicNames[1]));

    subscribeClient(*topicManager, *subscriptionManager, clientSession, topicNames);

    EXPECT_TRUE(clientSession->isSubscribed(topicNames[0]));
    EXPECT_TRUE(clientSession->isSubscribed(topicNames[1]));

    subscriptionManager->unsubscribe(topicManager->getTopic(topicNames[0]), clientSession.get());

    EXPECT_FALSE(clientSession->isSubscribed(topicNames[0]));
    EXPECT_TRUE(clientSession->isSubscribed(topicNames[1]));

    verifySubscribedTopics(*subscriptionManager, topicManager, topicNames[0], 0);
    verifySubscribedTopics(*subscriptionManager, topicManager, topicNames[1], 1);

    subscriptionManager->unsubscribe(topicManager->getTopic(topicNames[1]), clientSession.get());

    EXPECT_FALSE(clientSession->isSubscribed(topicNames[0]));
    EXPECT_FALSE(clientSession->isSubscribed(topicNames[1]));

    verifySubscribedTopics(*subscriptionManager, topicManager, topicNames[1], 0);

    clientSession->clearSession();
}
} // namespace

TEST_F(XMQ_StorageTests, unsubscribeClientFromTopic)
{
    const size_t officeIndexSerial = DateTime::Now().sinceEpoch().count();
    const auto   office1 = format("office{}", officeIndexSerial);
    const auto   office2 = format("office{}", officeIndexSerial + 1);

    const Strings topicNames({office1 + "/usb/1", office2 + "/vga/1"});
    unsubscribeTests(server(), topicNames);
}

TEST_F(XMQ_StorageTests, unsubscribeClientFromWildcards)
{
    const size_t officeIndexSerial = DateTime::Now().sinceEpoch().count();
    const auto   office1 = format("office{}", officeIndexSerial);
    const auto   office2 = format("office{}", officeIndexSerial + 1);

    const Strings topicNames({office1 + "/+/1", office2 + "/*"});
    unsubscribeTests(server(), topicNames);
}

TEST_F(XMQ_StorageTests, unsubscribeClientFromAll)
{
    size_t officeIndexSerial = DateTime::Now().sinceEpoch().count();

    const TestStorage storage(server());

    const auto topicManager = server()->getTopicManager();
    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto office1 = format("office{}", officeIndexSerial);
    officeIndexSerial++;
    const auto office2 = format("office{}", officeIndexSerial);

    const Strings topicNames({office1 + "/+/1", office1 + "/*", office2 + "/usb/2"});
    verifyIfSubscribed(storage.session(), topicNames, false);

    subscribeClient(*topicManager, *subscriptionManager, storage.session(), topicNames);
    verifyIfSubscribed(storage.session(), topicNames, true);

    for (const auto& topicName: topicNames)
    {
        size_t count = 0;
        subscriptionManager->find(topicManager->getTopic(topicName),
                                  [&count](const auto& subscriptionGroup)
                                  {
                                      for (const auto& [_, subscription]: subscriptionGroup)
                                      {
                                          count += subscription->clientCount();
                                      }
                                      return Subscriptions::ActionType::Continue;
                                  });
        EXPECT_EQ(1U, count);
    }

    storage.session()->unsubscribeAll();
    verifyIfSubscribed(storage.session(), topicNames, false);

    for (const auto& topicName: topicNames)
    {
        size_t count = 0;

        subscriptionManager->find(topicManager->getTopic(topicName),
                                  [&count](const auto& subscriptionGroup)
                                  {
                                      for (const auto& [_, subscription]: subscriptionGroup)
                                      {
                                          count += subscription->clientCount();
                                      }
                                      return Subscriptions::ActionType::Continue;
                                  });
        EXPECT_EQ(0U, count);
    }
}
