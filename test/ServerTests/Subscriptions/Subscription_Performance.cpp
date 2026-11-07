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

namespace {
void queueTopics(const vector<const Topic*>& topicList, SynchronizedQueue<const Topic*>& topics)
{
    for (const auto& topic: topicList)
    {
        topics.push_back(topic);
    }
}
} // namespace


TEST_F(XMQ_ServerTests, Subscription_Subscribe)
{
    const auto           storage = server()->getStorage();
    const auto           topicManager = server()->getTopicManager();
    SubscriptionManager  subscriptionManager(server().get(), topicManager, *logEngine());
    vector<const Topic*> topicList;

    constexpr auto size = 10U;

    for (size_t index = 0; index < size; ++index)
    {
        stringstream stream;
        stream << "test" << index << "/module" << index << "/device" << index;
        topicList.push_back(topicManager->getTopic(stream.str()));
    }

    for (const auto& topic: topicList)
    {
        subscriptionManager.subscribe(topic, nullptr, Qos::Qos0, 1, SubscriptionOptions());
    }

    size_t counter = 0;
    for (const auto& topic: topicList)
    {
        subscriptionManager.find(topic, [&counter](const SubscriptionGroup&)
                                 {
                                     counter++;
                                     return Subscriptions::ActionType::Continue;
                                 });
    }

    EXPECT_EQ(size, counter);

    for (const auto& topic: topicList)
    {
        subscriptionManager.unsubscribe(topic, nullptr);
        auto subscriptions = subscriptionManager.getSubscriptions(string(topic->fullName()));
        EXPECT_TRUE(subscriptions.empty());
    }
}

TEST_F(XMQ_ServerTests, Subscription_Shards)
{
    const auto          topicManager = server()->getTopicManager();
    SubscriptionManager subscriptionManager(server().get(), topicManager, *logEngine(), 16);
    vector<const Topic*> topicList;

    for (size_t index = 0; index < 128; ++index)
    {
        auto topic = topicManager->getTopic("test" + to_string(index) + "/module/device");
        topicList.push_back(topic);
        subscriptionManager.subscribe(topic, nullptr, Qos::Qos1, 1, SubscriptionOptions());
    }

    const auto wildcard = topicManager->getTopic("+/module/device");
    subscriptionManager.subscribe(wildcard, nullptr, Qos::Qos1, 1, SubscriptionOptions());
    const auto shared = topicManager->getTopic("$share/group/test0/module/device");
    subscriptionManager.subscribe(shared, nullptr, Qos::Qos1, 1, SubscriptionOptions());

    for (const auto& topic: topicList)
    {
        size_t matches = 0;
        subscriptionManager.find(topic,
                                 [&matches](const SubscriptionGroup&)
                                 {
                                     ++matches;
                                     return Subscriptions::ActionType::Continue;
                                 });
        EXPECT_EQ(2, matches);
        EXPECT_EQ(topic == topicList.front() ? 3 : 2, subscriptionManager.getSubscriptions(string(topic->fullName())).size());
    }

    EXPECT_EQ(130, subscriptionManager.getSubscriptions("#").size());

    for (const auto& topic: topicList)
    {
        subscriptionManager.unsubscribe(topic, nullptr);
    }
    subscriptionManager.unsubscribe(wildcard, nullptr);
    subscriptionManager.unsubscribe(shared, nullptr);
    EXPECT_TRUE(subscriptionManager.getSubscriptions("#").empty());
}

TEST_F(XMQ_ServerTests, Subscription_Performance)
{
    const auto           storage = server()->getStorage();
    const auto           topicManager = server()->getTopicManager();
    SubscriptionManager  subscriptionManager(server().get(), topicManager, *logEngine(), 16);
    vector<const Topic*> topicList;

    constexpr int    Size = 50;
    constexpr size_t TaskCount = 8;

    // Create subscriptions
    Stopwatch stopwatch;
    for (int i = 0; i < Size; ++i)
    {
        for (int j = 0; j < Size; ++j)
        {
            for (int k = 0; k < Size; ++k)
            {
                stringstream stream;
                stream << "test" << i << "/module" << j << "/device" << k;
                auto topic = topicManager->getTopic(stream.str());
                subscriptionManager.subscribe(topic, nullptr, Qos::Qos0, 1, SubscriptionOptions());
                topicList.push_back(topic);
            }
        }
    }
    stopwatch.stop();
    COUT("Creating " << setprecision(1) << fixed
                     << topicList.size() << " subscriptions took " << static_cast<int>(stopwatch.milliseconds()) << " ms. "
                     << "(" << static_cast<double>(topicList.size()) / stopwatch.seconds() / 1000 << "K/s)");

    SynchronizedQueue<const Topic*> topics;
    queueTopics(topicList, topics);

    atomic_size_t        counter = 0;
    vector<future<void>> tasks;
    for (size_t i = 0; i < TaskCount; ++i)
    {
        auto task = async(launch::async,
                          [&subscriptionManager, &topics, &counter]
                          {
                              const Topic* topic;
                              while (topics.pop_front(topic, 1ms))
                              {
                                  auto message = make_shared<mqtt::PublishMessage>(topic, string_view(""), static_cast<MessageId>(0));
                                  subscriptionManager.publishMessage(message);
                                  ++counter;
                              }
                          });
        tasks.push_back(std::move(task));
    }

    stopwatch.start();

    for (const auto& task: tasks)
    {
        task.wait();
    }

    stopwatch.stop();
    tasks.clear();

    EXPECT_EQ(topicList.size(), counter);

    COUT("Searching " << setprecision(1) << fixed
                      << topicList.size() << " subscriptions took " << static_cast<int>(stopwatch.milliseconds()) << " ms. "
                      << "(" << static_cast<double>(topicList.size()) / stopwatch.seconds() / 1000 << "K/s)");

    stopServers();
}
