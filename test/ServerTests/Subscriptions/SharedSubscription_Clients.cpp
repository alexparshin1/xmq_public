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
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

vector<shared_ptr<TestMqttClient>> createClients(const string&                baseClientId,
                                                 const size_t                 totalClients,
                                                 const shared_ptr<LogEngine>& logEngine)
{
    vector<shared_ptr<TestMqttClient>> clients;
    for (size_t index = 0; index < totalClients; ++index)
    {
        auto clientId = format("{}_{}", baseClientId, index);
        auto client = make_shared<TestMqttClient>(logEngine, clientId);
        clients.push_back(client);
    }
    return clients;
}

void subscribeClients(const vector<shared_ptr<TestMqttClient>>& clients,
                      const std::vector<string>&                topicNames,
                      const size_t                              totalMessageCount,
                      atomic_size_t&                            deliveredMessageCountTotal,
                      SynchronizedMap<string, size_t>&          deliveredMessageCountPerClient,
                      Semaphore&                                allMessagesReceived)
{
    const auto messageProperties = make_shared<MessageProperties>();
    size_t     topicIndex = 0;

    if (topicNames.empty())
    {
        return;
    }

    for (const auto& client: clients)
    {
        string clientId = client->getClientId();
        client->onMessage(
            [&deliveredMessageCountTotal, &allMessagesReceived, totalMessageCount, clientId, &deliveredMessageCountPerClient](const SPublishMessage&)
            {
                ++deliveredMessageCountTotal;
                deliveredMessageCountPerClient.modify(clientId, [](size_t& count)
                                                      {
                                                          ++count;
                                                      });
                if (deliveredMessageCountTotal == totalMessageCount)
                {
                    allMessagesReceived.post();
                }
            });
    }

    for (const auto& client: clients)
    {
        EXPECT_TRUE(client->isConnected());
        topicIndex = topicIndex % topicNames.size();
        const auto sharedDestination = Destination(client::MqttClient::getTopic(topicNames[topicIndex]), SubscriptionOptions(Qos::Qos0));
        client->subscribe(sharedDestination, messageProperties);
        topicIndex++;
    }
}

} // namespace

TEST_F(XMQ_ServerTests, SharedSubscription_ClientsToTopic)
{
    constexpr auto   totalClients = 10;
    constexpr size_t totalMessageCount = 10000;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    Semaphore     allMessagesReceived;
    atomic_size_t deliveredMessageCountTotal = 0;

    debugLog(false);

    SynchronizedMap<string, size_t> deliveredMessageCountPerClient;
    auto                            subscribers = createClients(subscriberClientId, totalClients, logEngine());

    subscribeClients(subscribers, {"$share/group1/" + topicName}, totalMessageCount,
                     deliveredMessageCountTotal,
                     deliveredMessageCountPerClient,
                     allMessagesReceived);

    TestMqttClient publisher(logEngine(), publisherClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(publisher.isConnected());

    Stopwatch stopwatch;

    for (size_t index = 0; index < totalMessageCount; ++index)
    {
        publisher.publish(topicName, "message", Qos::Qos0);
    }

    EXPECT_TRUE(allMessagesReceived.wait_for(5s));

    stopwatch.stop();
    COUT("Time elapsed: " << fixed << setprecision(1) << stopwatch.milliseconds() << " ms, "
                          << totalMessageCount / stopwatch.milliseconds() << "K messages per second");

    // Disconnect all clients:
    subscribers.clear();
    publisher.disconnect();

    deliveredMessageCountPerClient.for_each([](const auto& clientId, const auto& messageCount)
                                            {
                                                EXPECT_NEAR(static_cast<double>(totalMessageCount) / totalClients, static_cast<double>(messageCount), 5) << "Client: " << clientId << " received " << messageCount << " messages.";
                                                return true;
                                            });
}

TEST_F(XMQ_ServerTests, SharedSubscription_ClientsToWildcard)
{
    constexpr auto totalClients = 10;
    constexpr auto totalMessageCount = 1000;
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    Semaphore     allMessagesReceived;
    atomic_size_t deliveredMessageCountTotal = 0;

    debugLog(false);

    SynchronizedMap<string, size_t> deliveredMessageCountPerClient;
    auto                            subscribers = createClients(subscriberClientId, totalClients, logEngine());

    subscribeClients(subscribers, {"$share/group1/" + topicName + "/#"}, totalMessageCount, deliveredMessageCountTotal, deliveredMessageCountPerClient, allMessagesReceived);

    TestMqttClient publisher(logEngine(), publisherClientId, true, false, ProtocolVersion::MqttV31);
    EXPECT_TRUE(publisher.isConnected());

    Stopwatch stopwatch;

    for (size_t index = 0; index < totalMessageCount; ++index)
    {
        publisher.publish(topicName + "/1", "message", Qos::Qos0);
    }

    EXPECT_TRUE(allMessagesReceived.wait_for(5s));

    stopwatch.stop();
    COUT("Time elapsed: " << fixed << setprecision(1) << stopwatch.milliseconds() << " ms, "
                          << totalMessageCount / stopwatch.milliseconds() << "K messages per second");

    // Disconnect all clients:
    subscribers.clear();
    publisher.disconnect();

    deliveredMessageCountPerClient.for_each([](const auto& clientId, const auto& messageCount)
                                            {
                                                EXPECT_NEAR(static_cast<double>(totalMessageCount) / totalClients, static_cast<double>(messageCount), 5) << "Client: " << clientId << " received " << messageCount << " messages.";
                                                return true;
                                            });
}
