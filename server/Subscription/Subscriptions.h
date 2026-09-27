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

#include "base/FunctionRef.h"
#include "Subscription.h"
#include "storage/RedisStorage.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xmq {

class Subscriptions;

using USubscriptions = std::unique_ptr<Subscriptions>;

class XMQ_EXPORT Subscriptions
{
    friend class SubscriptionManager;

public:
    /**
     * Subscription map type
     */
    using Map = std::unordered_map<std::string_view, USubscriptions>;

    enum class ActionType
    {
        Continue,
        Remove
    };

    // Called while the search runs and never kept, so it is a reference, not a std::function:
    // the match action captures five words and allocated once per published message as one.
    using Action = FunctionRef<ActionType(SubscriptionGroup& subscriptionGroup)>;

    /**
     * Constructor
     * @param server            Server.
     * @param levelName         Subscription level.
     */
    explicit Subscriptions(Server* server, std::string_view levelName = "");

    Subscriptions(const Subscriptions&) = delete;
    Subscriptions(Subscriptions&&) = delete;
    Subscriptions& operator=(const Subscriptions&) = delete;
    Subscriptions& operator=(Subscriptions&&) = delete;

    /**
     * Clear all subscriptions
     */
    void clear();

    /**
     * Find subscriptions
     * @param topic             Topic path (exact or wildcard).
     * @param action            Callback function.
     * @param autoCreate        Auto create.
     */
    void find(const Topic* topic, const Action& action, bool autoCreate = false);

    /**
     * Deliver the message to all subscriptions
     * @param topic             Topic path (not a wildcard).
     * @param message           Message.
     * @param wildcardMatch     Wildcard match mode.
     * @param deliverToSessions Deliver to session ids.
     * @param deliverToBridges  Deliver to bridges.
     * @param dynamicRoute      Out: set true if the resolved route depends on per-message state
     *                          (shared or no-local subscriptions) and must not be cached.
     * @param domain            Message domain.
     */
    void matchSessionsForDelivery(const Topic*           topic,
                                  const PublishMessage&  message,
                                  bool                   wildcardMatch,
                                  mqtt::SubscriptionIds& deliverToSessions,
                                  cluster::NodeSet&      deliverToBridges,
                                  bool&                  dynamicRoute,
                                  MessageDomain          domain = MessageDomain::Client);

protected:
    /**
     * Subscribe to the topic
     * @param topic             Topic path (exact or wildcard).
     * @param clientSession     Client session.
     * @param qos               QoS.
     * @param subscriptionId    Subscription id.
     * @param options           Subscription options.
     */
    std::shared_ptr<Subscription> subscribe(const Topic* topic, const std::shared_ptr<ISubscriptionClient>& clientSession, Qos qos, uint32_t subscriptionId, SubscriptionOptions options);

    /**
     * Unsubscribe from the topic
     * @param topic             Topic path (exact or wildcard).
     * @param clientSession     Client session.
     * @param destroyed         Subscription destroyed.
     */
    void unsubscribe(const Topic* topic, ClientSession* clientSession, bool& destroyed);

    /**
     * Find one exact match of the topic path among the topic subscriptions
     * @param topic             Topic path (not a wildcard).
     * @param action            Callback function.
     * @param autoCreate        Auto create if not found.
     */
    void findExactMatch(const Topic* topic, const Action& action, bool autoCreate);

    /**
     * Find all matching wildcard subscriptions to the topic path among wildcard subscriptions
     * @param currentLevel      Current subscription level.
     * @param path              Topic path (not a wildcard).
     * @param level             Subscription level number in the topic path.
     * @param action            Callback function.
     */
    void findWildcardsMatchingTopic(Subscriptions* currentLevel, const std::vector<std::string_view>& path, size_t level,
                                    const Action& action);

    /**
     * @brief Find all matching topic subscriptions to the topic path among wildcard subscriptions.
     * @param currentLevel      Current subscription level.
     * @param path              Topic path (wildcard).
     * @param level             Subscription level number in the topic path.
     * @param action            Callback function.
     */
    void findWildcardMatches(Subscriptions* currentLevel, const std::vector<std::string_view>& path, size_t level,
                             const Action& action);

private:
    mutable sptk::ReadWriteMutex m_mutex;             ///< Mutex for protecting subscriptions
    std::string                  m_levelName;         ///< Subscription level name (exact or wildcard)
    Map                          m_subscriptions;     ///< Subscriptions map
    SubscriptionGroup            m_subscriptionGroup; ///< Client subscriptions by groups
    Server*                      m_server;            ///< Server.

    /**
     * @brief Add subscription.
     * @param topic                         Topic path.
     * @param subscriptionClient            Subscription client.
     * @param qos                           QoS.
     * @param subscriptionId                Subscription id.
     * @param options                       Subscription options.
     * @return Subscription.
     */
    std::shared_ptr<Subscription> add(const Topic* topic, const std::shared_ptr<ISubscriptionClient>& subscriptionClient, Qos qos, uint32_t subscriptionId, SubscriptionOptions options);

    /**
     * Find all subscriptions under the current level
     * @param currentLevel      Current subscription level.
     * @param action            Callback function.
     */
    static void findAll(Subscriptions* currentLevel, const Action& action);

    /**
     * Scan current level by wildcard
     * @param currentLevel      Current subscription level.
     * @param path              Topic path.
     * @param level             Subscription level number in the path.
     * @param action            Callback function.
     * @param levelName         Subscription level.
     * @param isMaxLevel        Is max level.
     */
    void scanCurrentLevelByWildcard(Subscriptions* currentLevel, const std::vector<std::string_view>& path, size_t level,
                                    const Action&    action,
                                    std::string_view levelName, bool isMaxLevel);
    /**
     * Scan the current level by name
     * @param currentLevel      Current subscription level.
     * @param path              Topic path.
     * @param level             Subscription level number in the path.
     * @param action            Callback function.
     * @param levelName         Subscription level.
     * @param isMaxLevel        Is max level.
     */
    void scanCurrentLevelByName(Subscriptions* currentLevel, const std::vector<std::string_view>& path, size_t level,
                                const Action& action, std::string_view levelName, bool isMaxLevel);

    void processMatchedLevel(const std::vector<std::string_view>& path, size_t level, Subscriptions* matchedSubscriptions,
                             const Action& action);
};

} // namespace xmq
