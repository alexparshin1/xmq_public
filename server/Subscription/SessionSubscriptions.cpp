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

#include "SessionSubscriptions.h"

#include <algorithm>

using namespace std;
using namespace sptk;
using namespace xmq;

void SessionSubscriptions::addClient(ISubscriptionClient* client, const SSessionSubscription& subscription)
{
    unique_lock lock(m_mutex);

    if (const auto it = m_clientIndex.find(client);
        it == m_clientIndex.end())
    {
        m_clientSessionSubscriptions.emplace_back(client, subscription);
        m_clientIndex[client] = prev(m_clientSessionSubscriptions.end());
    }
    else
    {
        // Re-subscribing replaces the entry, and erasing it invalidates every iterator naming it -
        // including the round-robin cursor a shared subscription delivers by. removeClient() steps
        // the cursor off the element it is about to erase; this path did not, so a client that
        // re-subscribed while it was the cursor left the cursor dangling. What followed was a
        // comparison against end() that is undefined, and then delivery through a freed node.
        //
        // Silent on libstdc++ and caught by MSVC's checked iterators: "list iterators
        // incompatible", from Delivery_AfterOverlappingReconnect_BridgeShape - an overlapping
        // reconnect is exactly a re-subscribe of a client that is already there.
        if (m_currentClientSessionSubscription == it->second)
        {
            ++m_currentClientSessionSubscription;
        }
        m_clientSessionSubscriptions.erase(it->second);
        m_clientSessionSubscriptions.emplace_back(client, subscription);
        it->second = prev(m_clientSessionSubscriptions.end());
    }

    if (m_currentClientSessionSubscription == m_clientSessionSubscriptions.end())
    {
        m_currentClientSessionSubscription = m_clientSessionSubscriptions.begin();
    }
}

void SessionSubscriptions::removeClient(ISubscriptionClient* client)
{
    unique_lock lock(m_mutex);

    const auto it = m_clientIndex.find(client);
    if (it == m_clientIndex.end())
    {
        return;
    }

    if (it->second == m_currentClientSessionSubscription)
    {
        ++m_currentClientSessionSubscription;
        if (m_currentClientSessionSubscription == m_clientSessionSubscriptions.end())
        {
            m_currentClientSessionSubscription = m_clientSessionSubscriptions.begin();
        }
    }

    m_clientSessionSubscriptions.erase(it->second);
    m_clientIndex.erase(it);

    if (m_clientIndex.empty())
    {
        m_currentClientSessionSubscription = m_clientSessionSubscriptions.end();
    }
}

void SessionSubscriptions::clear()
{
    unique_lock lock(m_mutex);
    m_clientSessionSubscriptions.clear();
    m_clientIndex.clear();
    m_currentClientSessionSubscription = m_clientSessionSubscriptions.end();
}

SSessionSubscription SessionSubscriptions::getSubscription(ISubscriptionClient* subscriptionClient) const
{
    shared_lock lock(m_mutex);

    const auto it = m_clientIndex.find(subscriptionClient);
    if (it == m_clientIndex.end())
    {
        return nullptr;
    }

    return it->second->subscription;
}

bool SessionSubscriptions::empty() const
{
    shared_lock lock(m_mutex);
    return m_clientIndex.empty();
}

size_t SessionSubscriptions::size() const
{
    shared_lock lock(m_mutex);
    return m_clientIndex.size();
}

bool SessionSubscriptions::any_of(const Predicate& predicate) const
{
    const shared_lock lock(m_mutex);
    return ranges::any_of(m_clientSessionSubscriptions,
                          [&predicate](const auto& entry)
                          {
                              return predicate(entry.client, entry.subscription);
                          });
}

void SessionSubscriptions::for_each(const Visitor& callback) const
{
    shared_lock lock(m_mutex);
    for (const auto& [client, subscription]: m_clientSessionSubscriptions)
    {
        callback(client, subscription);
    }
}

void SessionSubscriptions::for_next(const Visitor& callback, const Predicate& eligible)
{
    unique_lock lock(m_mutex);

    for (size_t tried = 0; tried < m_clientSessionSubscriptions.size(); ++tried)
    {
        if (m_currentClientSessionSubscription == m_clientSessionSubscriptions.end())
        {
            m_currentClientSessionSubscription = m_clientSessionSubscriptions.begin();
        }
        const auto chosen = m_currentClientSessionSubscription++;
        if (eligible(chosen->client, chosen->subscription))
        {
            callback(chosen->client, chosen->subscription);
            break;
        }
    }
    if (m_currentClientSessionSubscription == m_clientSessionSubscriptions.end())
    {
        m_currentClientSessionSubscription = m_clientSessionSubscriptions.begin();
    }
}

void SessionSubscriptions::for_next(const Visitor& callback)
{
    unique_lock lock(m_mutex);

    if (m_currentClientSessionSubscription != m_clientSessionSubscriptions.end())
    {
        callback(m_currentClientSessionSubscription->client, m_currentClientSessionSubscription->subscription);
        ++m_currentClientSessionSubscription;
        if (m_currentClientSessionSubscription == m_clientSessionSubscriptions.end())
        {
            m_currentClientSessionSubscription = m_clientSessionSubscriptions.begin();
        }
    }
}
