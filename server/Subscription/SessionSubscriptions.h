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

#include "SessionSubscription.h"
#include "base/FunctionRef.h"
#include "common/ISubscriptionClient.h"

#include <list>

namespace xmq {

class XMQ_EXPORT SessionSubscriptions final
{
public:
    SessionSubscriptions()
        : m_currentClientSessionSubscription(m_clientSessionSubscriptions.end())
    {
    }

    void addClient(ISubscriptionClient* client, const SSessionSubscription& subscription);

    /**
     * @brief Remove the client subscription.
     * @param client Subscription client.
     */
    void removeClient(ISubscriptionClient* client);

    void                 clear();
    SSessionSubscription getSubscription(ISubscriptionClient* client) const;

    bool   empty() const;
    size_t size() const;

    /// Called for each client while the lock is held and never kept - a reference, so that the
    /// per-message visit does not allocate a std::function for its three captured references.
    using Visitor = FunctionRef<void(ISubscriptionClient*, const SSessionSubscription&)>;

    void for_each(const Visitor& callback) const;
    void for_next(const Visitor& callback);

private:
    struct ClientSessionSubscription
    {
        ISubscriptionClient* client;
        SSessionSubscription subscription;
    };
    // A list, not a vector: m_clientIndex and m_currentClientSessionSubscription hold iterators into
    // it, and a list keeps them valid while other entries come and go. A vector invalidates every one
    // of them on a reallocating push_back and those after the erased element on erase - tried in
    // 4661fb9e, it crashed XMQ_ClusterTests.attachAndDetachToCluster in removeClient() and did not
    // move the Point-To-Point median.
    using ClientSessionSubscriptionList = std::list<ClientSessionSubscription>;
    mutable std::shared_mutex                                               m_mutex;                            ///< Mutex for thread-safe access to the data.
    ClientSessionSubscriptionList                                           m_clientSessionSubscriptions;       ///< The list of subscriptions.
    std::map<ISubscriptionClient*, ClientSessionSubscriptionList::iterator> m_clientIndex;                      ///< Subscribed clients iterators to the list of subscriptions.
    ClientSessionSubscriptionList::iterator                                 m_currentClientSessionSubscription; ///< Current client for the shared topic iterations.
};

} // namespace xmq
