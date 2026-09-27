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

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_F(XMQ_ServerTests, NodeSubscription_Subscribe)
{
    const auto storage = server()->getStorage();
    auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    auto       topicManager = server()->getTopicManager();
    const auto clusterTopics = make_shared<cluster::Topics>(topicManager);

    CServerNode nodeSettings;
    auto        nodeConnection = make_shared<cluster::ServerNode>(server().get(), nodeSettings, *clusterTopics);

    const auto subscriptionManager = server()->getSubscriptionManager();

    const auto* topic = const_cast<Topic*>(server()->getTopic("topic/1"));

    //auto*       subscription = subscriptions.subscribe(topic1, {}, Qos::Qos1, 0, SubscriptionOptions());
    //subscription->addBridgeConnection(bridgeConnection);
    subscriptionManager->find(topic, [](SubscriptionGroup&)
                              {
                                  return Subscriptions::ActionType::Continue;
                              },
                              true);
}
