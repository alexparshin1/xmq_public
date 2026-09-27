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
#include "server/MessageDelivery.h"
#include "storage/MessageDeliveryPacker.h"
#include <sptk5/threads/JoiningThread.h>

#include <gtest/gtest.h>

#include <mutex>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
double createPersistentSessions(Server* server, const size_t threadCount, SynchronizedQueue<int64_t>& clientIndexQueue)
{
    const auto             connectMessageProperties = make_shared<MessageProperties>();
    JoiningThreads         threads;
    vector<SClientSession> sessions;
    mutex                  sessionsMutex;

    Stopwatch stopwatch;
    stopwatch.start();

    threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; i++)
    {
        threads.emplace_back([&server, &clientIndexQueue, &sessions, &sessionsMutex, &connectMessageProperties]
                             {
                                 int64_t clientIndex = 0;
                                 while (!clientIndexQueue.empty() && clientIndexQueue.pop_front(clientIndex, 10ms))
                                 {
                                     const auto clientId = format("client-{}", clientIndex);

                                     const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
                                     connectMessageParameters->m_cleanSession = false;
                                     connectMessageParameters->setClientId(clientId);

                                     auto session = ClientSession::factory(server, connectMessageParameters,
                                                                           connectMessageProperties);

                                     const scoped_lock lock(sessionsMutex);
                                     sessions.push_back(std::move(session));
                                 }
                             });
    }

    threads.clear();
    stopwatch.stop();

    return stopwatch.milliseconds();
}
} // namespace

/**
 * @brief Test initialization of the new persistent client session.
 */
TEST_F(XMQ_StorageTests, initSessionPerformance)
{
    const auto initialClientIndex = chrono::system_clock::now().time_since_epoch().count();

    constexpr auto iterationCount = 200;
    constexpr auto threadCount = 32;

    SynchronizedQueue<int64_t> newClientIndexQueue;
    SynchronizedQueue<int64_t> existingClientIndexQueue;
    for (auto i = 0; i < iterationCount; i++)
    {
        newClientIndexQueue.push_back(initialClientIndex + i);
        existingClientIndexQueue.push_back(initialClientIndex + i);
    }

    // Stage 1: Create new sessions not yet existed in the storage:
    auto elapsedMs = createPersistentSessions(server().get(), threadCount, newClientIndexQueue);
    COUT("Created " << iterationCount << " new ClientSessions: " << fixed << setprecision(2) << iterationCount / elapsedMs << "K/s");

    // Stage 2: Load sessions created in Stage 1 from the storage:
    elapsedMs = createPersistentSessions(server().get(), threadCount, existingClientIndexQueue);
    COUT("Loaded " << iterationCount << " existing ClientSessions: " << fixed << setprecision(2) << iterationCount / elapsedMs << "K/s");
}

/**
 * @brief Test if packing and unpacking PublishMessage is correct.
 */
TEST_F(XMQ_StorageTests, PublishMessagePacker)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);

    const auto* topic = server()->getTopic("a/test/topic");
    Buffer      sourceMessagePayload;
    for (uint8_t i = 0; i < 128; i++)
    {
        sourceMessagePayload.append(i);
    }

    auto sourceMessage = make_shared<mqtt::PublishMessage>(topic, string_view(sourceMessagePayload.c_str(), sourceMessagePayload.size()));

    sourceMessage->setSourceNode("anode");

    auto sourceMessageProperties = make_shared<MessageProperties>();
    sourceMessageProperties->setProperty(Property::AssignedClientIdentifier, "test-client");
    sourceMessageProperties->setProperty(Property::MaximumQOS, 2);
    sourceMessageProperties->setUserProperty("user-property", "user-property-value");
    sourceMessage->setProperties(sourceMessageProperties);

    SubscriptionIdSet subscriptionIds {1234, 2345};

    auto messageDelivery = MessageDelivery::create(testStorage->session(), sourceMessage, Qos::Qos1,
                                                   0, subscriptionIds, false, 12345);

    auto packedMessage = MessageDeliveryPacker::pack(messageDelivery);
    ASSERT_FALSE(packedMessage.empty());

    auto unpackedMessageDelivery = MessageDeliveryPacker::unpack(testStorage->session(), packedMessage);
    auto unpackedMessage = dynamic_pointer_cast<PublishMessage>(unpackedMessageDelivery->message());

    ASSERT_TRUE(unpackedMessage);
    ASSERT_EQ(sourceMessage->destination()->fullName(), unpackedMessage->destination()->fullName());

    ASSERT_EQ(sourceMessagePayload, Buffer(unpackedMessage->payload()));

    ASSERT_EQ("anode", unpackedMessage->getSourceNode());

    const auto unpackedMessageProperties = unpackedMessage->getProperties();
    ASSERT_TRUE(unpackedMessageProperties);

    string_view clientId;
    ASSERT_TRUE(unpackedMessageProperties->getProperty(Property::AssignedClientIdentifier, clientId));
    ASSERT_EQ(clientId, "test-client");

    int64_t maxQos = 0;
    ASSERT_TRUE(unpackedMessageProperties->getProperty(Property::MaximumQOS, maxQos));
    ASSERT_EQ(maxQos, 2);

    const auto userPropertyValue = unpackedMessageProperties->getUserProperty("user-property");
    ASSERT_EQ(userPropertyValue, "user-property-value");
}

/**
 * @brief Test the performance of packing and unpacking PublishMessage.
 */
TEST_F(XMQ_StorageTests, MessageDeliveryPackerPerformance)
{
    const auto testStorage = make_shared<TestStorage>(server(), false);

    const auto* topic = server()->getTopic("a/test/topic");
    Buffer      sourceMessagePayload;
    for (uint8_t i = 0; i < 128; i++)
    {
        sourceMessagePayload.append(i);
    }

    const auto sourceMessage = make_shared<mqtt::PublishMessage>(topic, string_view(sourceMessagePayload.c_str(), sourceMessagePayload.size()));
    sourceMessage->setSourceNode("anode");

    const auto sourceMessageProperties = make_shared<MessageProperties>();
    sourceMessageProperties->setProperty(Property::AssignedClientIdentifier, "test-client");
    sourceMessageProperties->setProperty(Property::MaximumQOS, 2);
    sourceMessageProperties->setUserProperty("user-property", "user-property-value");
    sourceMessage->setProperties(sourceMessageProperties);

    SubscriptionIdSet subscriptionIds {1234, 2345};

    auto messageDelivery = MessageDelivery::create(testStorage->session(), sourceMessage, Qos::Qos1,
                                                   0, subscriptionIds, false, 12345);

    constexpr size_t iterations = 100000;

    //──────────────────────────────────────────────────────────────────────────────
    // Source message is ready, measure the packing performance:
    Stopwatch stopwatch;
    stopwatch.start();
    for (size_t i = 0; i < iterations; i++)
    {
        (void) MessageDeliveryPacker::pack(messageDelivery);
    }
    stopwatch.stop();
    COUT(format("MessageDeliveryPacker::pack():   {} calls took {:1.1f}ms ({:1.1f}K/s)", iterations, stopwatch.milliseconds(), iterations / stopwatch.milliseconds()));

    //──────────────────────────────────────────────────────────────────────────────
    // Source message buffer is ready, measure the packing performance:
    const auto packedMessage = MessageDeliveryPacker::pack(messageDelivery);
    stopwatch.start();
    for (size_t i = 0; i < iterations; i++)
    {
        (void) MessageDeliveryPacker::unpack(testStorage->session(), packedMessage);
    }
    stopwatch.stop();
    COUT(format("PublishMessagePacker::unpack(): {} calls took {:1.1f}ms ({:1.1f}K/s)", iterations, stopwatch.milliseconds(), iterations / stopwatch.milliseconds()));
}
