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

#include "SessionSubscription.h"
#include "ClientSession.h"
#include "Server.h"

using namespace std;
using namespace sptk;
using namespace xmq;

SessionSubscription::SessionSubscription(const std::shared_ptr<ISubscriptionClient>& clientSession,
                                         std::shared_ptr<Subscription>               subscription,
                                         const Qos                                   qos,
                                         const SubscriptionOptions                   subscriptionOptions,
                                         const uint32_t                              subscriptionId)
    : m_subId(subscriptionId)
    , m_qos(qos)
    , m_subscriptionOptions(subscriptionOptions)
    , m_clientSession(clientSession)
    , m_subscription(std::move(subscription))
{
}

std::shared_ptr<SessionSubscription> SessionSubscription::create(const std::shared_ptr<ISubscriptionClient>& subscriptionClient,
                                                                 std::shared_ptr<Subscription>               subscription,
                                                                 const Qos                                   qos,
                                                                 const SubscriptionOptions                   subscriptionOptions,
                                                                 const uint32_t                                    subscriptionId)
{
    auto* sessionSubscriptionPtr = new SessionSubscription(subscriptionClient, std::move(subscription), qos, subscriptionOptions, subscriptionId);
    auto  sessionSubscription = std::shared_ptr<SessionSubscription>(sessionSubscriptionPtr);
    return sessionSubscription;
}

RecordId SessionSubscription::sessionRecordId() const
{
    return m_clientSession->recordId();
}
