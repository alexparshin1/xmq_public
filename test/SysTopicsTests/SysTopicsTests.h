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

#pragma once

#include "test/ServerTests/ServerTests.h"
#include "test/TestMqttClient.h"

namespace xmq {

class XMQ_EXPORT XMQ_SysTopicsTests : public XMQ_ServerTests
{
protected:
    static std::shared_ptr<TestMqttClient>                                                           createTestSubscriber();
    static std::tuple<std::shared_ptr<TestMqttClient>, std::shared_ptr<TestMqttClient>, std::string> createTestSubscriberAndPublisher();

    /**
     * @brief Waits for a broker counter to reach the value expected of it, and returns what it
     *        last read.
     *
     * The counters move after the client has its CONNACK and on another thread, so reading one
     * straight after a client reports itself connected is a race - the test wins it on an idle
     * machine and loses it on a busy one. That is what made these tests depend on the order they
     * ran in: shuffling changed what had warmed the broker up beforehand, not what the broker did.
     *
     * Waiting rather than sleeping also states the intent. A sleep says "this takes about 100ms",
     * which is a guess about a machine; this says "the value is expected to arrive", which is what
     * the test actually means, and it returns as soon as it does.
     *
     * @param topicKind Counter to read.
     * @param expected  Value it should reach.
     * @param timeout   How long to allow before giving up and returning what it holds, so the
     *                  caller's EXPECT_EQ reports the real value rather than a timeout.
     */
    static uint64_t waitForCounter(SystemStatistics::SysTopicKind topicKind, uint64_t expected,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * @brief Waits for the broker to have no connected clients before the test starts.
     *
     * These tests read counters that belong to the broker, not to them, and the broker is shared
     * with every other test in the suite. A client from the test before is still on its way out
     * while this one takes its baseline, so the baseline moves under it: the count read 1, then 2
     * as this test's client arrived, then 1 again as the previous one left, and an assertion about
     * "one more than before" saw whichever of those it happened to catch.
     *
     * Differencing does not help when the thing being differenced is still moving. Waiting for
     * quiet does, and it costs nothing when the previous test cleaned up promptly.
     */
    void SetUp() override;
};

} // namespace xmq
