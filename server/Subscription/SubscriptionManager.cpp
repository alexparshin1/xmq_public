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

#include "SubscriptionManager.h"
#include "ClientSession.h"
#include "Server.h"
#include "base/MessageProperties.h"
#include "common/mqtt/PublishMessage.h"
#include "server/Cluster/Cluster.h"

#include <ranges>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/**
 * @brief The message as it goes to the node behind a cluster link: a copy that names the shared
 *        subscriptions assigned to that node.
 *
 * A copy, properties included: the message is shared by every recipient, and none of the others
 * is to see what was assigned to this one.
 */
SMessage withClusterShares(const xmq::PublishMessage& message, const vector<string>& shares)
{
    const auto* published = dynamic_cast<const mqtt::PublishMessage*>(&message);
    if (published == nullptr)
    {
        return {};
    }
    auto copy = make_shared<mqtt::PublishMessage>(*published);
    const auto& original = message.getProperties();
    auto        properties = original ? make_shared<MessageProperties>(dynamic_cast<const MessageProperties&>(*original))
                                      : make_shared<MessageProperties>();
    for (const auto& share: shares)
    {
        properties->setUserProperty(xmq::PublishMessage::ClusterShareProperty, share);
    }
    copy->setProperties(properties);
    return copy;
}

} // namespace

SubscriptionManager::SubscriptionManager(Server* server, STopicManager topicManager, LogEngine& logEngine)
    : m_topicManager(std::move(topicManager))
    , m_logger(logEngine, "SubscriptionManager ")
    , m_topics(server)
    , m_wildcards(server)
    , m_server(server)
{
}

size_t SubscriptionManager::publishMessage(const shared_ptr<PublishMessage>& message, const MessageDomain domain)
{
    // Find all the sessions that should receive the message
    mqtt::SubscriptionIds deliverToSessions;
    cluster::NodeSet      deliverToBridges;

    if (!matchMessage(message, domain, deliverToSessions, deliverToBridges))
    {
        return 0;
    }

    const auto deliverCount = deliverShard(message, deliverToSessions, 0, 1);

    for (auto& clusterNode: deliverToBridges)
    {
        clusterNode->publish(message);
    }

    return deliverCount;
}

bool SubscriptionManager::matchMessage(const std::shared_ptr<PublishMessage>& message, const MessageDomain domain,
                                       mqtt::SubscriptionIds& deliverToSessions, cluster::NodeSet& deliverToBridges)
{
    const auto& msg = *message;

    if (domain == MessageDomain::Client && msg.destination()->isSystem())
    {
        // Client can't publish to system topics
        return false;
    }

    const auto* destination = msg.destination();

    // A retained message belongs to the topic it was published to, so it is recorded here - before
    // any subscription is consulted, and whether or not anyone is subscribed. Publishing an empty
    // payload with the retain flag clears it, which is MQTT's own way of saying "forget this".
    // A publication forwarded by another cluster node is left alone: its node commits the change and
    // sends it with its revision, and only that copy can be ordered against the others.
    if (msg.isRetain() && !msg.isFromCluster())
    {
        const auto topicName = std::string(destination->fullName());
        // $SYS is published from the moment the statistics start - before the server has a cluster
        // to ask about, so it is not asked.
        const auto cluster = m_server != nullptr && !destination->isSystem() ? m_server->getCluster() : nullptr;
        auto*      retainedSync = cluster ? cluster->retainedSync() : nullptr;
        if (destination->isSystem())
        {
            // $SYS is each node's own and regenerated continuously: neither shared nor persisted -
            // that would be a write per counter update, forever, of values stale once read.
            (void) m_retainedMessages.set(topicName, msg.payload(), msg.getQos());
        }
        else if (retainedSync != nullptr)
        {
            // The cluster's: every node holds it, whether or not anyone there is subscribed.
            retainedSync->publish(topicName, msg.payload(), msg.getQos());
        }
        else if (const auto change = m_retainedMessages.set(topicName, msg.payload(), msg.getQos());
                 change != RetainedMessages::Change::None)
        {
            // Storage before statistics, and deliberately so: the in-memory store has already
            // changed, and a counter that throws must not leave the persisted copy behind - that is
            // how a cleared retained message used to come back after a restart.
            if (m_server != nullptr)
            {
                m_retainedMessages.store(m_server->getStorage(), m_server->getNodeName(), topicName);
            }
            if (domain == MessageDomain::Client)
            {
                countRetainedChange(change);
            }
        }
    }

    auto dynamicRoute = false;
    m_topics.matchSessionsForDelivery(destination, msg, false, deliverToSessions, deliverToBridges, dynamicRoute, domain);
    m_wildcards.matchSessionsForDelivery(destination, msg, true, deliverToSessions, deliverToBridges, dynamicRoute, domain);

    // A session matched by an exact topic and by a wildcard was listed twice above.
    deliverToSessions.finish();

    return true;
}

void SubscriptionManager::reassignShared(const vector<pair<weak_ptr<Subscription>, SMessage>>& messages,
                                         const ISubscriptionClient*                               endedSession)
{
    for (const auto& [sharedSubscription, message]: messages)
    {
        const auto subscription = sharedSubscription.lock();
        const auto publishMessage = dynamic_pointer_cast<PublishMessage>(message);
        if (!subscription || !publishMessage)
        {
            continue;
        }
        // Chosen as any message is - the next member, or a node with members - with the one that
        // ended left out. No member left, and it goes nowhere, as a message nobody subscribes to does.
        mqtt::SubscriptionIds deliverToSessions;
        cluster::NodeSet      notForwarded;
        bool                  dynamicRoute = false;
        subscription->matchSessionsForDelivery(*publishMessage, deliverToSessions, notForwarded, dynamicRoute,
                                               MessageDomain::Server, endedSession);
        deliverToSessions.finish();
        deliverShard(publishMessage, deliverToSessions, 0, 1);
    }
}

void SubscriptionManager::countRetainedChange(const RetainedMessages::Change change) const
{
    if (const auto statistics = m_server ? m_server->systemStatistics() : nullptr)
    {
        if (change == RetainedMessages::Change::Added)
        {
            statistics->increment(SystemStatistics::SysTopicKind::BrokerMessagesRetainedCount);
        }
        else if (change == RetainedMessages::Change::Removed)
        {
            statistics->decrement(SystemStatistics::SysTopicKind::BrokerMessagesRetainedCount);
        }
    }
}

size_t SubscriptionManager::deliverShard(const std::shared_ptr<PublishMessage>& message, const mqtt::SubscriptionIds& deliverToSessions,
                                         const size_t shardIndex, const size_t shardCount)
{
    size_t deliverCount = 0;

    for (const auto& [session, info]: deliverToSessions)
    {
        if (shardCount > 1 && shardOf(session, shardCount) != shardIndex)
        {
            continue;
        }
        // Retain As Published means the message keeps the retain flag it was published with, not
        // that it becomes retained. Passing the option itself marks every delivery to such a
        // subscription as retained, and a bridge then stores each one as the retained message for
        // its topic - so every later subscriber is greeted by the last thing that crossed.
        const auto retain = info.m_options.m_retainAsPublished && message->isRetain();
        if (!info.m_clusterShares.empty())
        {
            const auto assigned = withClusterShares(*message, info.m_clusterShares);
            info.m_session->postMessage(assigned ? assigned : message, info.m_qos, info.m_ids, retain);
        }
        else if (!info.m_sharedSubscription.expired())
        {
            info.m_session->postMessage(message, info.m_qos, info.m_ids, retain, info.m_sharedSubscription);
        }
        else
        {
            info.m_session->postMessage(message, info.m_qos, info.m_ids, retain);
        }
        ++deliverCount;
    }

    return deliverCount;
}

shared_ptr<Subscription> SubscriptionManager::subscribe(const Topic*                                topic,
                                                        const std::shared_ptr<ISubscriptionClient>& clientSession,
                                                        const Qos                                   qos,
                                                        const uint32_t                              subscriptionId,
                                                        const SubscriptionOptions                   options)
{
    SSubscription subscription;

    if (topic->isWildcard())
    {
        // Wildcard subscription can't start from '$', unless it's $SYS, $CLUSTER, or $share.
        if (topic->isSystem() || topic->isCluster() || topic->isShared() || topic->name()[0] != '$')
        {
            subscription = m_wildcards.subscribe(topic, clientSession, qos, subscriptionId, options);
        }
        else
        {
            m_logger.error("Wildcard subscription can't start from '$', unless it's $SYS or $share");
        }
    }
    else
    {
        subscription = m_topics.subscribe(topic, clientSession, qos, subscriptionId, options);
    }

    if (m_server && m_server->systemStatistics())
    {
        m_server->systemStatistics()->setValue(SystemStatistics::SysTopicKind::BrokerSubscriptionsCount, m_topicManager->size());
    }

    return subscription;
}

void SubscriptionManager::unsubscribe(const Topic* topic, ClientSession* clientSession)
{
    bool destroyed = false;
    if (topic->isWildcard())
    {
        m_wildcards.unsubscribe(topic, clientSession, destroyed);
    }
    else
    {
        m_topics.unsubscribe(topic, clientSession, destroyed);
    }

    if (auto* statistics = m_server ? m_server->systemStatistics() : nullptr)
    {
        statistics->setValue(SystemStatistics::SysTopicKind::BrokerSubscriptionsCount, m_topicManager->size());
    }
}

void SubscriptionManager::clear()
{
    m_topics.clear();
    m_wildcards.clear();
}

void SubscriptionManager::find(const Topic* topic, const Subscriptions::Action& action, const bool autoCreate, const bool exactMatch)
{
    if (topic->isWildcard())
    {
        if (!exactMatch)
        {
            m_topics.find(topic, action);
        }
        m_wildcards.find(topic, action);
    }
    else
    {
        m_topics.find(topic, action, autoCreate);
        if (!exactMatch)
        {
            m_wildcards.findWildcardsMatchingTopic(&m_wildcards, topic->path(), 0, action);
        }
    }
}

SSubscription SubscriptionManager::find(const int subscriptionRecordId)
{
    const lock_guard lock(m_mutex);
    if (const auto itor = m_subscriptionIndex.find(subscriptionRecordId);
        itor != m_subscriptionIndex.end())
    {
        return itor->second;
    }
    return nullptr;
}

map<const Topic*, weak_ptr<Subscription>, less<>> SubscriptionManager::getSubscriptions(const std::string& topicName)
{
    map<const Topic*, weak_ptr<Subscription>, less<>> result;
    const Topic*                                      topic = m_topicManager->getTopic(topicName);

    find(topic,
         [&result](const SubscriptionGroup& subscriptionGroup)
         {
             for (const auto& subscription: views::values(subscriptionGroup))
             {
                 result[subscription->getTopic()] = subscription;
             }
             return Subscriptions::ActionType::Continue;
         });

    return result;
}
