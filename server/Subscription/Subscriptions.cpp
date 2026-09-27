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

#include "Subscriptions.h"
#include "../Server.h"
#include "ClientSession.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

shared_ptr<Subscription> Subscriptions::subscribe(const Topic*                                topic,
                                                  const std::shared_ptr<ISubscriptionClient>& clientSession,
                                                  const Qos                                   qos,
                                                  const uint32_t                              subscriptionId,
                                                  const SubscriptionOptions                   options)
{
    SSubscription subscription;
    if (!topic->isWildcard())
    {
        auto* changeTopic = const_cast<Topic*>(topic);
        subscription = changeTopic->getSubscription().lock();
        if (!subscription)
        {
            subscription = add(topic, clientSession, qos, subscriptionId, options);
            changeTopic->setSubscription(subscription);
        }
        else
        {
            subscription->addSubscriptionClient(clientSession, qos,
                                                subscriptionId, options);
        }
    }
    else
    {
        subscription = add(topic, clientSession, qos,
                           subscriptionId, options);
    }

    if (clientSession != nullptr)
    {
        clientSession->subscribedTo(subscription, qos, options);
    }

    return subscription;
}

void Subscriptions::unsubscribe(const Topic* topic, ClientSession* clientSession, bool& destroyed)
{
    const ReadWriteLock lock(m_mutex, ReadWriteLock::Mode::Writer);

    find(topic,
         [&clientSession, &destroyed, &topic](SubscriptionGroup& subscriptions)
         {
             if (const auto iterator = subscriptions.find(topic);
                 iterator != subscriptions.end())
             {
                 const auto& subscription = iterator->second;
                 subscription->removeSubscriptionClient(clientSession);
                 if (clientSession)
                 {
                     clientSession->unsubscribedFrom(*subscription);
                 }
                 if (subscription->empty())
                 {
                     destroyed = true;
                     return ActionType::Remove;
                 }
                 return ActionType::Continue;
             }
             return ActionType::Remove;
         });
}

shared_ptr<Subscription> Subscriptions::add(const Topic*                                topic,
                                            const std::shared_ptr<ISubscriptionClient>& subscriptionClient,
                                            const Qos                                   qos,
                                            const uint32_t                              subscriptionId,
                                            const SubscriptionOptions                   options)
{
    try
    {
        const ReadWriteLock lock(m_mutex, ReadWriteLock::Mode::Reader);

        auto* currentLevel = this;

        for (const auto& levelName: topic->path())
        {
            auto& currentLevelSubscriptions = currentLevel->m_subscriptions;
            auto  levelIterator = currentLevelSubscriptions.find(levelName);
            if (levelIterator == currentLevelSubscriptions.end())
            {
                lock.upgradeToWriteLock();
                auto              subscriptions = make_unique<Subscriptions>(m_server, levelName);
                const string_view key = subscriptions->m_levelName;
                const auto&       [itor, result] =
                    currentLevelSubscriptions.try_emplace(key, std::move(subscriptions));
                levelIterator = itor;
            }
            currentLevel = levelIterator->second.get();
        }

        // Find a subscription for this topic's group
        SSubscription subscription;
        auto&         currentLevelSubscriptionGroup = currentLevel->m_subscriptionGroup;
        if (const auto iterator = currentLevelSubscriptionGroup.find(topic);
            iterator == currentLevelSubscriptionGroup.end())
        {
            subscription = Subscription::create(m_server, topic, currentLevelSubscriptionGroup);
            currentLevelSubscriptionGroup.try_emplace(topic, subscription);
        }
        else
        {
            subscription = iterator->second;
        }

        if (subscriptionClient != nullptr)
        {
            subscription->addSubscriptionClient(subscriptionClient, qos, subscriptionId, options);
        }

        return subscription;
    }
    catch (const Exception& e)
    {
        CERR(e.what());
    }
    return nullptr;
}

void Subscriptions::matchSessionsForDelivery(const Topic*           topic,
                                             const PublishMessage&  message,
                                             const bool             wildcardMatch,
                                             mqtt::SubscriptionIds& deliverToSessions,
                                             cluster::NodeSet&      deliverToBridges,
                                             bool&                  dynamicRoute,
                                             MessageDomain          domain)
{
    // KNOWN ISSUE, deliberately left as-is: when the message is retained, findExactMatch() below
    // is called with autoCreate true and inserts into m_subscriptions while only this reader lock
    // is held. add() gets the same situation right by calling upgradeToWriteLock() immediately
    // before it inserts.
    //
    // Taking the writer lock here instead was tried and reverted: $SYS metrics are retained and
    // published continuously, so every one of them took the tree exclusively and starved delivery
    // (Topics_SysSubscriptionToAll / _ToWildcard both fail). The correct fix is to upgrade inside
    // findExactMatch() only when it actually inserts, which needs the lock passed down to it.
    const ReadWriteLock lock(m_mutex, ReadWriteLock::Mode::Reader);

    const auto action =
        [&message, &deliverToSessions, &deliverToBridges, &dynamicRoute, domain](SubscriptionGroup& subscriptionGroup)
    {
        for (const auto& subscription: subscriptionGroup | views::values)
        {
            subscription->matchSessionsForDelivery(message, deliverToSessions, deliverToBridges, dynamicRoute, domain);
        }
        return ActionType::Continue;
    };

    if (wildcardMatch)
    {
        findWildcardsMatchingTopic(this, topic->path(), 0, action);
    }
    else
    {
        findExactMatch(topic, action, message.isRetain());
    }
}

void Subscriptions::find(const Topic* topic, const Action& action, const bool autoCreate)
{
    if (topic->isWildcard())
    {
        findWildcardMatches(this, topic->path(), 0, action);
    }
    else
    {
        findExactMatch(topic, action, autoCreate);
    }
}

void Subscriptions::findExactMatch(const Topic* topic, const Action& action, const bool autoCreate)
{

    if (const auto existingSubscription = topic->getSubscription().lock();
        existingSubscription && !topic->isShared())
    {
        if (auto& subscriptionGroup = existingSubscription->subscriptionGroup();
            !subscriptionGroup.empty())
        {
            if (const auto result = action(subscriptionGroup);
                result == ActionType::Remove)
            {
                // Remove requested
                subscriptionGroup.erase(topic);
            }
        }
        return;
    }

    auto* currentLevel = this;

    for (const auto& levelName: topic->path())
    {
        auto levelIterator = currentLevel->m_subscriptions.find(levelName);
        if (levelIterator == currentLevel->m_subscriptions.end())
        {
            if (autoCreate)
            {
                auto              subscriptions = make_unique<Subscriptions>(m_server, levelName);
                const string_view key = subscriptions->m_levelName;
                const auto&       [itor, result] =
                    currentLevel->m_subscriptions.emplace(key, std::move(subscriptions));
                levelIterator = itor;
            }
            else
            {
                return;
            }
        }

        currentLevel = levelIterator->second.get();
    }

    if (!currentLevel->m_subscriptionGroup.empty())
    {
        if (const auto result = action(currentLevel->m_subscriptionGroup);
            result == ActionType::Remove)
        {
            // Remove requested
            currentLevel->m_subscriptionGroup.erase(topic);
        }
    }
    else if (autoCreate)
    {
        auto subscription = Subscription::create(m_server, topic, currentLevel->m_subscriptionGroup);
        currentLevel->m_subscriptionGroup.try_emplace(topic, subscription);
        action(currentLevel->m_subscriptionGroup);
    }
}

void Subscriptions::findWildcardMatches(Subscriptions* currentLevel, const std::vector<std::string_view>& path, const size_t level,
                                        const Action&  action)
{
    const size_t maxLevel = path.size() - 1;

    const auto& levelName = path[level];
    const bool  isMaxLevel = level == maxLevel;
    if (levelName[0] == '#' || levelName[0] == '+')
    {
        scanCurrentLevelByWildcard(currentLevel, path, level, action, levelName, isMaxLevel);
    }
    else
    {
        scanCurrentLevelByName(currentLevel, path, level, action, levelName, isMaxLevel);
    }
}

void Subscriptions::scanCurrentLevelByName(Subscriptions*    currentLevel, const vector<std::string_view>& path, const size_t level,
                                           const Action&     action,
                                           const string_view levelName, const bool isMaxLevel)
{
    const auto levelIterator = currentLevel->m_subscriptions.find(levelName);
    if (levelIterator == currentLevel->m_subscriptions.end())
    {
        return;
    }

    if (isMaxLevel)
    {
        if (const auto result = action(levelIterator->second->m_subscriptionGroup);
            result == ActionType::Remove)
        {
            // Remove requested
            currentLevel->m_subscriptions.erase(levelIterator);
        }
        return;
    }

    findWildcardMatches(levelIterator->second.get(), path, level + 1, action);
}

void Subscriptions::scanCurrentLevelByWildcard(Subscriptions*    currentLevel, const vector<std::string_view>& path,
                                               const size_t      level, const Action&                          action,
                                               const string_view levelName, const bool                         isMaxLevel)
{
    const auto& subscriptions = currentLevel->m_subscriptions;

    if (isMaxLevel && levelName[0] == '#')
    {
        findAll(currentLevel, action);
        return;
    }

    const auto subscriptionsEnd = subscriptions.end();
    const auto matchMaxLevel = isMaxLevel && levelName[0] != '#';
    for (auto levelIterator = subscriptions.begin(); levelIterator != subscriptionsEnd;)
    {
        if (matchMaxLevel)
        {
            if (const auto result = action(levelIterator->second->m_subscriptionGroup);
                result == ActionType::Remove)
            {
                // Remove requested
                levelIterator = currentLevel->m_subscriptions.erase(levelIterator);
                continue;
            }
        }
        else
        {
            if (levelName == "+")
            {
                findWildcardMatches(levelIterator->second.get(), path, level + 1, action);
            }
            else
            {
                findAll(levelIterator->second.get(), action);
            }
        }
        ++levelIterator;
    }
}

void Subscriptions::findWildcardsMatchingTopic(Subscriptions* currentLevel, const vector<std::string_view>& path,
                                               const size_t   level, const Action&                          action)
{
    const auto& levelName = path[level];

    const auto isSystemTopic = level == 0 && levelName[0] == '$' &&
                               (strncasecmp(levelName.data(), "$sys", 4) == 0 || strncasecmp(levelName.data(), "$cluster", 8) == 0);

    for (const array<string_view, 3> matchNames = {"#", "+", levelName};
         const auto&                 matchName: matchNames)
    {
        if (auto iterator = currentLevel->m_subscriptions.find(matchName);
            iterator != currentLevel->m_subscriptions.end())
        {
            // MQTT 5, 4.7.2: a topic filter starting with a wildcard must not match a topic
            // name beginning with '$'. That covers '+' as well as '#' - only a filter naming
            // the level literally, such as "$SYS/broker/#", may match one. isSystemTopic is
            // set at level 0 only, so this excludes leading wildcards and nothing else.
            if (isSystemTopic && (iterator->first[0] == '#' || iterator->first[0] == '+'))
            {
                continue;
            }

            if (iterator->first[0] == '#')
            {
                action(iterator->second->m_subscriptionGroup);
                continue;
            }

            processMatchedLevel(path, level, iterator->second.get(), action);
        }
    }
}

void Subscriptions::processMatchedLevel(const vector<std::string_view>& path, const size_t                  level,
                                        Subscriptions*                  matchedSubscriptions, const Action& action)
{
    if (const auto maxLevel = path.size() - 1;
        level == maxLevel)
    {
        if (!matchedSubscriptions->m_subscriptionGroup.empty())
        {
            action(matchedSubscriptions->m_subscriptionGroup);
        }
    }
    else
    {
        findWildcardsMatchingTopic(matchedSubscriptions, path, level + 1, action);
    }
}

void Subscriptions::findAll(Subscriptions* currentLevel, const Action& action)
{
    const auto& levelSubscriptions = currentLevel->m_subscriptions;
    for (auto levelIterator = levelSubscriptions.begin(); levelIterator != levelSubscriptions.end();)
    {
        if (const auto& subscriptions = levelIterator->second;
            !subscriptions->m_subscriptionGroup.empty())
        {
            if (const auto result = action(levelIterator->second->m_subscriptionGroup);
                result == ActionType::Remove)
            {
                // Remove requested
                levelIterator = currentLevel->m_subscriptions.erase(levelIterator);
                continue;
            }
        }

        //if (!currentLevel->m_subscriptions.empty())
        {
            findAll(levelIterator->second.get(), action);
        }

        ++levelIterator;
    }
}


Subscriptions::Subscriptions(Server* server, const std::string_view levelName)
    : m_levelName(levelName)
      , m_server(server)
{
}

void Subscriptions::clear()
{
    const ReadWriteLock lock(m_mutex, ReadWriteLock::Mode::Writer);
    for (const auto& subscription: views::values(m_subscriptionGroup))
    {
        subscription->removeSubscriptionClients();
    }
    m_subscriptionGroup.clear();
    m_subscriptions.clear();
}
