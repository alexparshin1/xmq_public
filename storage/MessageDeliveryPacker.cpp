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

#include "MessageDeliveryPacker.h"
#include "server/Server.h"

using namespace std;
using namespace sptk;
using namespace xmq;

Buffer MessageDeliveryPacker::pack(const SMessageDelivery& messageDelivery)
{
    Buffer packed;

    const auto message = dynamic_pointer_cast<PublishMessage>(messageDelivery->message());
    if (!message)
    {
        throw Exception("Only Publish message is supported");
    }

    write(packed, message->destination()->fullName());
    write(packed, message->payloadData(), message->payloadSize());
    write(packed, message->isRetain());
    write(packed, message->getSourceNode());
    if (const auto properties = message->getProperties())
    {
        write(packed, true);
        write(packed, properties);
    }
    else
    {
        write(packed, false);
    }

    return packed;
}

SMessageDelivery MessageDeliveryPacker::unpack(const SClientSession& session, const Buffer& sourceData)
{
    auto state = startUnpacking(sourceData);

    string_view topicName;
    read(state, topicName);
    const auto* destination = session->server().getTopic(topicName);

    string_view payload;
    read(state, payload);

    auto isRetain {false};
    read(state, isRetain);

    string_view sourceNode;
    read(state, sourceNode);

    auto hasProperties {false};
    read(state, hasProperties);

    const auto message = make_shared<mqtt::PublishMessage>(destination, payload, static_cast<MessageId>(0), isRetain);
    message->setSourceNode(sourceNode);

    if (hasProperties)
    {
        const auto properties = make_shared<MessageProperties>();
        read(state, *properties);
        message->setProperties(properties);
    }

    // TODO: Add restoring of QOS, subscriptionIds, retain flag, and record id.
    SubscriptionIdSet subscriptionIds {};
    auto              messageDelivery = MessageDelivery::create(session, message, Qos::Qos1,
                                                                0, subscriptionIds, false, 12345);

    return messageDelivery;
}

void MessageDeliveryPacker::write(Buffer& packed, const SMessageProperties& properties)
{
    const auto originalBufferSize = packed.bytes();
    const auto expectedPropertiesSize = properties->expectedSize();

    packed.reserve(originalBufferSize + expectedPropertiesSize + sizeof(uint32_t) + 1);
    packed.append(Marker_Properties);
    packed.append(expectedPropertiesSize);

    auto* tail = packed.data() + packed.bytes();
    properties->write(tail, {});

    packed.bytes(packed.bytes() + expectedPropertiesSize);
}
