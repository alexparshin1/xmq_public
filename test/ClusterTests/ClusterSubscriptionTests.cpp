/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "test/ClusterTests/ClusterTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Check that cluster peers keep one effective entry per local filter, even when multiple
 * persistent client sessions use it. Removing one client must retain the entry, and disconnecting
 * the last client must retain it while its persistent session is offline. A separate filter is
 * unsubscribed explicitly to verify that removal propagates when no local client still uses it.
 */
TEST_F(XMQ_ClusterTests, effectiveSubscriptionsAreSharedAndDeduplicated)
{
    auto [primary, secondary] = makeClusterOfTwoNodes();
    const auto [clientId1, clientId2, sharedFilter] = makeTestNames();
    const string removedFilter = sharedFilter + "/removed";

    auto connectSubscriber = [](const Host& host, const string& id)
    {
        auto client = make_shared<client::MqttClient>();
        EXPECT_EQ(ReasonCode::Success,
                  client->connect(host, ConnectCredentials(id, "user", "secret"),
                                  {.m_cleanSession = false}, ProtocolVersion::MqttV5));
        return client;
    };
    auto first = connectSubscriber(m_primaryServerHost, clientId1);
    auto second = connectSubscriber(m_primaryServerHost, clientId2);
    ASSERT_TRUE(first->isConnected());
    ASSERT_TRUE(second->isConnected());
    first->subscribe(sharedFilter);
    second->subscribe(sharedFilter);
    first->subscribe(removedFilter);

    auto hasFilter = [&]
    {
        const auto local = primary->getCluster()->getNodeSubscriptions("primary");
        const auto filters = secondary->getCluster()->getNodeSubscriptions("primary");
        return local.contains(sharedFilter) && local.contains(removedFilter) &&
               filters.contains(sharedFilter) && filters.contains(removedFilter);
    };
    for (int i = 0; i < 100 && !hasFilter(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(hasFilter()) << "Subscription snapshot did not reach the peer";

    first->unsubscribe(Destination(client::MqttClient::getTopic(sharedFilter)));
    first->unsubscribe(Destination(client::MqttClient::getTopic(removedFilter)));
    auto removedExplicitly = [&]
    {
        const auto filters = secondary->getCluster()->getNodeSubscriptions("primary");
        return filters.contains(sharedFilter) && !filters.contains(removedFilter);
    };
    for (int i = 0; i < 100 && !removedExplicitly(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(removedExplicitly()) << "Unsubscribe did not update the shared filter set";

    second->disconnect();
    const auto persistedOffline = [&]
    {
        return secondary->getCluster()->getNodeSubscriptions("primary").contains(sharedFilter);
    };
    for (int i = 0; i < 100 && !persistedOffline(); ++i)
    {
        this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(persistedOffline()) << "Offline persistent session lost its effective subscription";
}
