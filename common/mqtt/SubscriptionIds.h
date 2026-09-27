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

#include "base/Message.h"
#include "base/SubscriptionIdSet.h"
#include "common/ISubscriptionClient.h"
#include "common/SubscriptionOptions.h"

#include <set>
#include <utility>
#include <vector>

namespace xmq {

class ClientSession;

namespace mqtt {

/**
 * @brief Represents a collection of subscription identifiers used
 *        for returning matched subscription ids with the message.
 */
class XMQ_EXPORT SubscriptionIds final
{
public:
    struct SessionInfo
    {
        Qos                                  m_qos {Qos::Invalid};
        SubscriptionOptions                  m_options;
        SubscriptionIdSet                    m_ids;
        std::shared_ptr<ISubscriptionClient> m_session; ///< Keeps the session alive until the (possibly deferred) delivery is done.
    };

    /// One entry per session, ordered by session address once finish() has run - the order the map
    /// this replaced iterated in, so sharding by session is unchanged.
    using Entry = std::pair<ISubscriptionClient*, SessionInfo>;
    using Entries = std::vector<Entry>;

    SubscriptionIds() = default;

    /**
     * @brief Record that a session matched, with the subscription it matched through.
     *
     * Appends. A session matched through several subscriptions is listed once per match until
     * finish() merges them.
     */
    bool add(const std::shared_ptr<ISubscriptionClient>& session, Qos qos, uint32_t subscriptionId, SubscriptionOptions options);

    /**
     * @brief Merge the matches of each session into one entry, and order entries by session.
     *
     * The merged entry keeps what the first match recorded and gathers every subscription id, in the
     * order they were added - what the map this replaced did on insert. Must run before the entries
     * are read.
     */
    void finish();

    /**
     * @brief Drop every entry, keeping the storage for the next message.
     *
     * Releases the sessions the entries hold, which is why a reused instance must be cleared as soon
     * as its message is delivered rather than when the next one arrives.
     */
    void clear() noexcept
    {
        m_sessions.clear();
    }

    Entries::iterator begin()
    {
        return m_sessions.begin();
    }

    Entries::iterator end()
    {
        return m_sessions.end();
    }

    [[nodiscard]] Entries::const_iterator begin() const
    {
        return m_sessions.begin();
    }

    [[nodiscard]] Entries::const_iterator end() const
    {
        return m_sessions.end();
    }

    [[nodiscard]] size_t size() const
    {
        return m_sessions.size();
    }

private:
    // A vector, not the std::map it was: a map allocates a node for every session a message reaches,
    // and a Point-To-Point message reaches one, so the node was a whole allocation per message. The
    // vector is reused across messages by the delivery threads and stops allocating once it is sized.
    Entries m_sessions;
};

}; // namespace mqtt
} // namespace xmq
