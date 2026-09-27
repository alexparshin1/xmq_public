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

#include "MessageDispatch.h"

using namespace std;
using namespace sptk;
using namespace xmq;

MessageDispatch::MessageDispatch(const SMessage& message, Qos qos, const MessageId messageId, bool duplicated,
                                 bool setRetainFlag)
    : m_message(message)
    , m_deliveryId(messageId)
    , m_flags({qos, duplicated, setRetainFlag})
{
}

MessageDispatch::MessageDispatch(const SMessage& message, Qos qos, const MessageId messageId, bool setRetainFlag,
                                 const SubscriptionIdSet& subscriptionIds)
    : m_message(message)
    , m_deliveryId(messageId)
    , m_flags({qos, false, setRetainFlag})
    , m_subscriptionIds(subscriptionIds)
{
}
