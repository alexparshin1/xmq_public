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

#include "client/MqttClient.h"

#include <chrono>
#include <memory>

namespace xmq::test {

/**
 * @brief Subscribes, and waits until the broker has actually registered it.
 *
 * MqttClient::subscribe() only sends the SUBSCRIBE; the SUBACK arrives later and on another
 * thread. A test that publishes straight afterwards is therefore racing the broker's registration
 * of its own subscription - the publish legitimately matches nothing, is never delivered, and the
 * failure is indistinguishable from a message the broker lost.
 *
 * Rare on an idle machine and not rare at all on a loaded one, which is why it shows up as a test
 * that passes everywhere and fails on one image of a farm run. It has been diagnosed three times
 * now under three different names, so the helper lives here rather than in whichever file needed
 * it last.
 *
 * @return False if no SUBACK arrived within the timeout, which is worth asserting on: what follows
 *         would otherwise measure this race rather than whatever the test is about.
 */
[[nodiscard]] inline bool subscribeAndWait(const std::shared_ptr<client::MqttClient>& client,
                                           const Destination&                         destination,
                                           const SMessageProperties&                  properties = {},
                                           std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    sptk::Semaphore subscribed;

    client->onAck([&subscribed](const SMessage& ack)
                  {
                      if (ack && ack->is(Message::Type::SubscribeAck))
                      {
                          subscribed.post();
                      }
                  });

    client->subscribe(destination, properties);
    const auto acknowledged = subscribed.wait_for(timeout);

    // Put back as it was found: the callback above captures a semaphore that is about to go out of
    // scope, and a later ack finding it there would be a use after free.
    client->onAck({});

    return acknowledged;
}

/// Several topics in one SUBSCRIBE, which is still one SUBACK.
[[nodiscard]] inline bool subscribeAndWait(const std::shared_ptr<client::MqttClient>& client,
                                           const Destinations&                        destinations,
                                           const SMessageProperties&                  properties = {},
                                           std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    sptk::Semaphore subscribed;

    client->onAck([&subscribed](const SMessage& ack)
                  {
                      if (ack && ack->is(Message::Type::SubscribeAck))
                      {
                          subscribed.post();
                      }
                  });

    client->subscribe(destinations, properties);
    const auto acknowledged = subscribed.wait_for(timeout);

    client->onAck({});

    return acknowledged;
}

/// The common case: one topic, one QoS.
[[nodiscard]] inline bool subscribeAndWait(const std::shared_ptr<client::MqttClient>& client,
                                           const std::string&                         topicName,
                                           const Qos                                  qos = Qos::Qos1)
{
    return subscribeAndWait(client, Destination(client::MqttClient::getTopic(topicName),
                                                SubscriptionOptions(qos)));
}

} // namespace xmq::test
