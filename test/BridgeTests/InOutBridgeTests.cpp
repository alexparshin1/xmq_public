/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "test/BridgeTests/BridgeTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

// The bridge under test is configured "inout", so one bridge carries both directions at once.
// inBridge and OutBridge each exercise a single direction; these check that enabling both does
// not break either, and that a message crosses exactly once rather than being echoed back.

TEST_F(XMQ_BridgeTests, inOutBridge_CarriesBothDirections)
{
    waitForBridge();

    // Publish on the other server, subscribe on this one.
    auto [inSubscriber, inPublisher, inTopicName] =
        createTestSubscriberAndPublisher(m_otherServerHost, m_xmqServerHost);

    // ...and the opposite way, over the same bridge.
    auto [outSubscriber, outPublisher, outTopicName] =
        createTestSubscriberAndPublisher(m_xmqServerHost, m_otherServerHost);

    Semaphore inboundReceived;
    inSubscriber->onMessage([&inboundReceived](const SMessage&)
                            {
                                inboundReceived.post();
                            });

    Semaphore outboundReceived;
    outSubscriber->onMessage([&outboundReceived](const SMessage&)
                             {
                                 outboundReceived.post();
                             });

    inPublisher->publish(client::MqttClient::getTopic(inTopicName), Buffer("inbound"), Qos::Qos1);
    outPublisher->publish(client::MqttClient::getTopic(outTopicName), Buffer("outbound"), Qos::Qos1);

    constexpr auto deliveryTimeout = 5s;
    EXPECT_TRUE(inboundReceived.wait_for(deliveryTimeout)) << "Message wasn't carried inbound";
    EXPECT_TRUE(outboundReceived.wait_for(deliveryTimeout)) << "Message wasn't carried outbound";
}

TEST_F(XMQ_BridgeTests, inOutBridge_DeliversEachMessageOnce)
{
    waitForBridge();

    // Subscribe on the bridging server, publish on the far one: the message crosses inbound. If
    // the bridge fed it back out again it would return here repeatedly, so counting deliveries
    // over a settle period is what catches a loop.
    auto [subscriber, publisher, topicName] =
        createTestSubscriberAndPublisher(m_otherServerHost, m_xmqServerHost);

    atomic_size_t deliveredCount = 0;
    Semaphore     firstDelivery;
    subscriber->onMessage([&deliveredCount, &firstDelivery](const SMessage&)
                          {
                              if (++deliveredCount == 1)
                              {
                                  firstDelivery.post();
                              }
                          });

    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("once"), Qos::Qos1);

    constexpr auto deliveryTimeout = 5s;
    ASSERT_TRUE(firstDelivery.wait_for(deliveryTimeout)) << "Message wasn't carried at all";

    // A loop would keep producing copies, so give one a chance to appear.
    constexpr auto settleTime = 500ms;
    this_thread::sleep_for(settleTime);

    EXPECT_EQ(1U, deliveredCount.load()) << "Message was delivered more than once - bridged traffic is looping";
}
