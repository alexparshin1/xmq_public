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

#include "common/SubscribeAckMessage.h"
#include "test/ServerTests/ServerTests.h"

#include <future>

using namespace std;
using namespace sptk;
using namespace xmq;

#ifdef XMQ_ACL_LIBRARY
namespace {
uint8_t subscribeResult(client::MqttClient& client, const string& filter)
{
    promise<uint8_t> result;
    client.onAck([&result](const SMessage& message)
                 {
                     if (const auto ack = dynamic_pointer_cast<SubscribeAckMessage>(message);
                         ack && !ack->subscriptionResults().empty())
                     {
                         result.set_value(ack->subscriptionResults().front());
                     }
                 });
    client.subscribe(Destination(client::MqttClient::getTopic(filter)));
    return result.get_future().get();
}
} // namespace

// mosquitto#3772: the shared-subscription group is not part of the filter a client is allowed to
// read. The ACL must be asked about "sensors/#", not "$share/group/sensors/#", or every shared
// subscription is refused by a rule that was meant to allow it.
TEST_P(XMQ_ServerTests, SharedSubscriptionIsCheckedAgainstItsFilter)
{
    const Host serverHost("localhost", TestTcpPortNumber);

    client::MqttClient       client(logEngine());
    const ConnectCredentials credentials {"shared-acl", "user", "secret"};
    ASSERT_EQ(ReasonCode::Success,
              client.connect(serverHost, credentials, {.m_cleanSession = true}, GetParam()));

    EXPECT_LE(subscribeResult(client, "$share/group/allowed/#"), 2)
        << "a shared subscription to an allowed filter must be granted";

    EXPECT_GT(subscribeResult(client, "$share/group/denied/#"), 2)
        << "a shared subscription to a denied filter must be refused at SUBSCRIBE";

    client.disconnect();
}
#endif
