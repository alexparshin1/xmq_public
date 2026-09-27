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

#include "common/DisconnectMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "base/MessageProperties.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

const Topic* createTestTopic(const SSubscriptionManager& subscriptionManager, const string& topicName)
{
    auto       found = false;
    const auto topic = subscriptionManager->getTopic(topicName);

    subscriptionManager->find(topic, [&found](const SubscriptionGroup&)
                              {
                                  found = true;
                                  return Subscriptions::ActionType::Continue;
                              },
                              true);
    EXPECT_TRUE(found);
    return topic;
}

/**
 * @brief Removes a test topic from a subscription manager if it exists.
 * @param subscriptionManager A shared pointer to the subscription manager containing topics and their subscriptions.
 * @param topicName The name of the topic to be removed.
 */
void removeTestTopic(const SSubscriptionManager& subscriptionManager, const string& topicName)
{
    auto       found = false;
    const auto topic = subscriptionManager->getTopic(topicName);

    subscriptionManager->find(topic, [&found, topic](const SubscriptionGroup&)
                              {
                                  COUT("Removing topic: " << topic->fullName());
                                  found = true;
                                  return Subscriptions::ActionType::Remove;
                              },
                              false, true);
}

} // namespace

TEST_F(XMQ_ServerTests, SubscriptionManager_AutoCreate)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto subscriptionManager = server()->getSubscriptionManager();

    removeTestTopic(subscriptionManager, topicName);

    auto       found = false;
    const auto topic = subscriptionManager->getTopic(topicName);

    subscriptionManager->find(topic, [&found, topic](const SubscriptionGroup&)
                              {
                                  COUT("Found topic: " << topic->fullName());
                                  found = true;
                                  return Subscriptions::ActionType::Continue;
                              },
                              false, true);
    EXPECT_FALSE(found);

    subscriptionManager->find(topic, [&found](const SubscriptionGroup&)
                              {
                                  found = true;
                                  return Subscriptions::ActionType::Continue;
                              },
                              true, true);
    EXPECT_TRUE(found);
}

TEST_F(XMQ_ServerTests, SubscriptionManager_CreateAndRemove)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto subscriptionManager = server()->getSubscriptionManager();

    createTestTopic(subscriptionManager, topicName);
    removeTestTopic(subscriptionManager, topicName);
}

TEST_F(XMQ_ServerTests, SubscriptionManager_FindByWildcard)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto subscriptionManager = server()->getSubscriptionManager();
    const auto simpleTopicName = String(topicName).replace("/", "-");

    const auto* topic = createTestTopic(subscriptionManager, topicName);
    const auto* simpleTopic = createTestTopic(subscriptionManager, simpleTopicName);

    set<string_view> found;
    const auto       wildcard = subscriptionManager->getTopic("#");
    subscriptionManager->find(wildcard, [&found, topic, simpleTopic](const SubscriptionGroup& subscriptionGroup)
                              {
                                  if (subscriptionGroup.contains(topic))
                                  {
                                      found.insert(topic->fullName());
                                  }
                                  if (subscriptionGroup.contains(simpleTopic))
                                  {
                                      found.insert(simpleTopic->fullName());
                                  }
                                  return Subscriptions::ActionType::Continue;
                              },
                              true);
    EXPECT_EQ(2U, found.size());

    removeTestTopic(subscriptionManager, topicName);
}
