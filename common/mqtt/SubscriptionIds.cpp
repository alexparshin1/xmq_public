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

#include "SubscriptionIds.h"

#include <algorithm>

using namespace std;

namespace xmq::mqtt {

bool SubscriptionIds::add(const std::shared_ptr<ISubscriptionClient>& session, const Qos qos, const uint32_t subscriptionId, const SubscriptionOptions options,
                          const std::weak_ptr<Subscription>& sharedSubscription)
{
    if (!session)
    {
        return false;
    }

    auto& [client, info] = m_sessions.emplace_back();
    client = session.get();
    info.m_qos = qos;
    info.m_options = options;
    info.m_session = session;
    if (!sharedSubscription.expired())
    {
        info.m_sharedSubscriptions.push_back(sharedSubscription);
    }
    if (subscriptionId != 0)
    {
        info.m_ids.push_back(subscriptionId);
        return true;
    }
    return false;
}

void SubscriptionIds::addClusterShare(const std::shared_ptr<ISubscriptionClient>& session, const Qos qos, const uint32_t subscriptionId,
                                      const SubscriptionOptions options, const std::string_view share,
                                      const std::weak_ptr<Subscription>& sharedSubscription)
{
    if (!session)
    {
        return;
    }
    add(session, qos, subscriptionId, options, sharedSubscription);
    m_sessions.back().second.m_clusterShares.emplace_back(share);
}

void SubscriptionIds::addClusterSession(const std::shared_ptr<ISubscriptionClient>& link, const std::string& clientId)
{
    if (!link)
    {
        return;
    }
    add(link, Qos::Qos1, 0, SubscriptionOptions(Qos::Qos1));
    m_sessions.back().second.m_clusterSessions.push_back(clientId);
}

void SubscriptionIds::removeClient(const std::string_view clientId)
{
    std::erase_if(m_sessions, [clientId](const Entry& entry) { return entry.first->getClientIdUnlocked() == clientId; });
}

bool SubscriptionIds::hasClient(const std::string_view clientId) const
{
    return std::ranges::any_of(m_sessions, [clientId](const Entry& entry) { return entry.first->getClientIdUnlocked() == clientId; });
}

void SubscriptionIds::finish()
{
    if (m_sessions.size() < 2)
    {
        // Nothing to order or merge - which is every Point-To-Point message.
        return;
    }

    // Stable, so that among the matches of one session the first added stays first and its QoS and
    // options are the ones kept, as they were when the first insert into the map won.
    std::ranges::stable_sort(m_sessions, {}, &Entry::first);

    auto kept = m_sessions.begin();
    for (auto next = kept + 1; next != m_sessions.end(); ++next)
    {
        if (next->first == kept->first)
        {
            auto& ids = kept->second.m_ids;
            ids.insert(ids.end(), next->second.m_ids.begin(), next->second.m_ids.end());
            // One copy goes to the node behind a link, so it carries every subscription assigned to it.
            auto& shares = kept->second.m_clusterShares;
            shares.insert(shares.end(), next->second.m_clusterShares.begin(), next->second.m_clusterShares.end());
            auto& sessions = kept->second.m_clusterSessions;
            sessions.insert(sessions.end(), next->second.m_clusterSessions.begin(), next->second.m_clusterSessions.end());
            // A cluster link carries the message for each shared subscription it was chosen for, and a
            // link that breaks has each offered again. A client matched another way too - or for
            // another shared subscription as well - would have had the message anyway: it is not one
            // subscription's to hand to another member.
            auto& shared = kept->second.m_sharedSubscriptions;
            if (kept->first->isClusterLink())
            {
                shared.insert(shared.end(), next->second.m_sharedSubscriptions.begin(), next->second.m_sharedSubscriptions.end());
            }
            else
            {
                shared.clear();
            }
        }
        else if (++kept != next)
        {
            *kept = std::move(*next);
        }
    }
    m_sessions.erase(kept + 1, m_sessions.end());

    mergeLinksOfOneNode();
}

void SubscriptionIds::mergeLinksOfOneNode()
{
    // A node has several links to another, and sends it a message once: over the link of the
    // topic, which is the one a subscription of its subscribers is on. The shared subscriptions
    // assigned to the node are on the first link, whatever the topic, and travel with the message
    // as properties, so they go over the link of the topic as well - the order of a topic's
    // messages is the order of one link.
    for (size_t i = 0; i < m_sessions.size();)
    {
        auto& [shareLink, shareInfo] = m_sessions[i];
        if (!shareLink->isClusterLink() || shareInfo.m_clusterShares.empty())
        {
            ++i;
            continue;
        }
        const auto origin = shareLink->bridgeOrigin();
        const auto target = std::ranges::find_if(m_sessions,
                                                 [&](const Entry& entry)
                                                 {
                                                     return entry.first != shareLink && entry.first->isClusterLink() &&
                                                            entry.first->bridgeOrigin() == origin;
                                                 });
        if (target == m_sessions.end())
        {
            ++i;
            continue;
        }
        auto& info = target->second;
        info.m_ids.insert(info.m_ids.end(), shareInfo.m_ids.begin(), shareInfo.m_ids.end());
        info.m_clusterShares.insert(info.m_clusterShares.end(), shareInfo.m_clusterShares.begin(), shareInfo.m_clusterShares.end());
        info.m_clusterSessions.insert(info.m_clusterSessions.end(), shareInfo.m_clusterSessions.begin(), shareInfo.m_clusterSessions.end());
        info.m_sharedSubscriptions.insert(info.m_sharedSubscriptions.end(), shareInfo.m_sharedSubscriptions.begin(),
                                          shareInfo.m_sharedSubscriptions.end());
        m_sessions.erase(m_sessions.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

} // namespace xmq::mqtt
