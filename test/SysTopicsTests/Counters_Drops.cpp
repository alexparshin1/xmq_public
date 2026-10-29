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
#include "test/SubscribeAndWait.h"
#include "test/SysTopicsTests/SysTopicsTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Restores the queue limits when the test ends, however it ends. The broker is shared with every
/// other test in the suite, and a limit left at one message would drop the traffic of the next one.
struct QueueLimitsGuard
{
    explicit QueueLimitsGuard(const shared_ptr<Settings>& settings)
        : m_settings(settings)
    {
        m_maxSize = settings->m_queue_limits.m_max_size.asInteger();
        m_maxBytes = settings->m_queue_limits.m_max_bytes.asInteger();
        m_maxInflight = settings->m_queue_limits.m_max_inflight_messages.asInteger();
    }

    ~QueueLimitsGuard()
    {
        m_settings->m_queue_limits.m_max_size = m_maxSize;
        m_settings->m_queue_limits.m_max_bytes = m_maxBytes;
        m_settings->m_queue_limits.m_max_inflight_messages = m_maxInflight;
    }

private:
    shared_ptr<Settings> m_settings;
    int64_t              m_maxSize {0};
    int64_t              m_maxBytes {0};
    int64_t              m_maxInflight {0};
};

} // namespace

// $SYS/broker/messages/publish/dropped and the control API field of the same name are fed from
// here. Both read 0 for every broker until 1.0.2, whatever the queue limits dropped: the counter
// existed, was published, and nothing incremented it, so the only trace a limit had done anything
// was a warning line in the log. A limit an operator cannot watch is a limit they cannot tune.
TEST_F(XMQ_SysTopicsTests, PublishDropped_QueueFull)
{
    using enum SystemStatistics::SysTopicKind;

    const auto limits = QueueLimitsGuard(server()->getSettings());

    // One message behind the window. A session whose client has gone away fills the window first -
    // ReceiveMaximum messages that will be sent when it returns - and then queues one more, so the
    // case is the one the limits were written for: the client is not slow, it is absent, and
    // nothing will take the messages until it comes back.
    server()->getSettings()->m_queue_limits.m_max_size = 1;
    server()->getSettings()->m_queue_limits.m_max_bytes = 0;

    const auto* statistics = server()->systemStatistics();
    const auto  initial = statistics->getValue(BrokerMessagesPublishDropped);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // An observer on the $SYS topic itself: the counter is meant to be read by an operator watching
    // the broker, not by a library call, so the test checks what the topic reports as well.
    const auto  sysTopic = SystemStatistics::sysTopicKindToTopic(BrokerMessagesPublishDropped);
    auto        sysValue = make_shared<atomic_uint64_t>(0);
    auto        sysObserver = make_shared<TestMqttClient>(logEngine(), publisherClientId + "-sys", true, false, ProtocolVersion::MqttV31);
    ASSERT_TRUE(sysObserver->isConnected());
    ASSERT_TRUE(test::subscribeAndWait(sysObserver, Destination(client::MqttClient::getTopic(sysTopic), SubscriptionOptions(Qos::Qos1))));
    sysObserver->onMessage([sysValue](const SPublishMessage& message)
                           {
                               sysValue->store(stoull(string(message->payload())), memory_order_relaxed);
                           });

    auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, false, false, ProtocolVersion::MqttV31);
    ASSERT_TRUE(subscriber->isConnected());
    ASSERT_TRUE(test::subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))));
    subscriber->disconnect();

    const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, ProtocolVersion::MqttV31);
    ASSERT_TRUE(publisher->isConnected());

    const auto acknowledged = make_shared<Semaphore>();
    publisher->onAck([acknowledged](const SMessage& message)
                     {
                         if (message->is(Message::Type::PublishAck))
                         {
                             acknowledged->post();
                         }
                     });

    // The publisher's acknowledgement says the broker took the message, not that it delivered it:
    // the drops happen on the delivery side, to a session with no client.
    constexpr size_t publishedCount = 200;
    for (size_t i = 0; i < publishedCount; ++i)
    {
        publisher->publish(topicName, "dropped", Qos::Qos1);
        ASSERT_TRUE(acknowledged->wait_for(2s)) << "no PUBACK for message " << i;
    }

    // How many fit depends on ReceiveMaximum, which the settings decide; what matters is that the
    // difference is not zero and never more than was published. The arithmetic of the limits
    // themselves is asserted on the queue directly, in InflightQueueTests.
    const auto dropped = waitForCounter(BrokerMessagesPublishDropped, initial + publishedCount / 2);
    EXPECT_LT(initial, dropped) << "the broker dropped " << publishedCount << " messages into a one-message queue and counted none";
    EXPECT_GE(initial + publishedCount, dropped) << "more drops counted than messages published";

    // The scan publishes a changed metric once a second; the last value it publishes is the total.
    const auto deadline = chrono::steady_clock::now() + 5s;
    while (sysValue->load(memory_order_relaxed) != dropped && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(dropped, sysValue->load(memory_order_relaxed)) << "the topic " << sysTopic << " did not report the drop count";

    sysObserver->onMessage({});
    sysObserver->disconnect();
    publisher->onAck({});
    publisher->disconnect();
}
