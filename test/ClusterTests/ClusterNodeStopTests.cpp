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

#include "TestOptions.h"
#include "test/ClusterTests/ClusterTests.h"
#include "test/SubscribeAndWait.h"

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * Test that cluster nodes know about each other after the startup.
 */
TEST_F(XMQ_ClusterTests, stopClusterNode)
{
    constexpr auto nodeCount = 2u;
    constexpr auto firstPort = 1890;

    auto nodes = makeTestCluster(nodeCount, firstPort);

    COUT("──────────────────────────[Cluster nodes started]───────────────────────────────────────────");

    auto                      publisher = make_shared<client::MqttClient>();
    ConnectCredentials        credentials("publisher", "user", "secret");
    client::ConnectParameters params {.m_cleanSession = false};
    // Not nodes[0]->listenerHosts()[0].hostname(): that is the bind address, 0.0.0.0, which is not
    // a usable connect target on Windows.
    Host                      host0("localhost", firstPort);
    publisher->connect(host0, credentials, params);

    ConnectCredentials credentials2("subscriber", "user", "secret");
    Host               host1(host0.hostname(), firstPort + 1);
    auto               subscriber = make_shared<client::MqttClient>();
    subscriber->connect(host1, credentials2, params);
    Counter counter;

    subscriber->onMessage([&counter](const SPublishMessage& message)
                          {
                              COUT("Received message: " << message->toString());
                              ++counter;
                          });

    ASSERT_TRUE(test::subscribeAndWait(subscriber, "test/topic"))
        << "The broker did not acknowledge the subscription";
    subscriber->disconnect();

    publisher->publish(client::MqttClient::getTopic("test/topic"), Buffer("Hello, World!"), Qos::Qos1);

    this_thread::sleep_for(100ms);

    subscriber->connect(host0, credentials2, params);

    this_thread::sleep_for(100ms);
    EXPECT_EQ(1, counter.get());

    COUT("──────────────────────────[Session populated]───────────────────────────────────────────────");

    nodes[0]->stop();

    COUT("──────────────────────────[Cluster node stopped]────────────────────────────────────────────");
}
