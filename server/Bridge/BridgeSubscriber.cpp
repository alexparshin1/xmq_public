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

#include "BridgeSubscriber.h"

#include "common/mqtt/PublishMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

MessageId BridgeSubscriber::postMessage(const SMessage& message, Qos /*qos*/, const SubscriptionIdSet& /*subscriptionIds*/, const bool /*retain*/)
{
    if (!message || !message->is(Message::Type::Publish))
    {
        return 0;
    }

    if (m_forward)
    {
        m_forward(dynamic_pointer_cast<mqtt::PublishMessage>(message));
    }

    return 0;
}
