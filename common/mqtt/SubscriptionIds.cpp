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

bool SubscriptionIds::add(const std::shared_ptr<ISubscriptionClient>& session, const Qos qos, const uint32_t subscriptionId, const SubscriptionOptions options)
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
    if (subscriptionId != 0)
    {
        info.m_ids.push_back(subscriptionId);
        return true;
    }
    return false;
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
        }
        else if (++kept != next)
        {
            *kept = std::move(*next);
        }
    }
    m_sessions.erase(kept + 1, m_sessions.end());
}

} // namespace xmq::mqtt
