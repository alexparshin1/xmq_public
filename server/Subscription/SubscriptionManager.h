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

#include "RetainedMessages.h"
#include "Subscriptions.h"

namespace xmq {

class XMQ_EXPORT SubscriptionManager final
{
public:
    /**
     * @brief Constructor.
     * @param server            Server.
     * @param topicManager      Topic manager.
     * @param logEngine         Log engine.
     */
    explicit SubscriptionManager(Server* server, STopicManager topicManager, sptk::LogEngine& logEngine);

    SubscriptionManager(const SubscriptionManager&) = delete;
    SubscriptionManager(SubscriptionManager&&) = delete;
    SubscriptionManager& operator=(const SubscriptionManager&) = delete;
    SubscriptionManager& operator=(SubscriptionManager&&) = delete;

    /**
     * @brief Destructor.
     */
    ~SubscriptionManager() = default;

    const Topic* getTopic(const std::string& topicName) const
    {
        return m_topicManager->getTopic(topicName);
    }

    void          find(const Topic* topic, const Subscriptions::Action& action, bool autoCreate = false, bool exactMatch = false);
    SSubscription find(int subscriptionRecordId);
    auto&         subscriptionIndex()
    {
        return m_subscriptionIndex;
    }

    /**
     * @brief Publish message.
     * @param message           Message.
     * @param domain            Message domain: restricts publishing of the client messages.
     * @return                  Number of subscribers.
     */
    size_t publishMessage(const std::shared_ptr<PublishMessage>& message, MessageDomain domain = MessageDomain::Client);

    /**
     * @brief Resolve the sessions and bridges a message should be delivered to, without delivering it.
     * @param message           Message.
     * @param domain            Message domain: restricts publishing of the client messages.
     * @param deliverToSessions Out: matched sessions.
     * @param deliverToBridges  Out: matched cluster bridges.
     * @return                  False if the message may not be published (client publish to a system topic).
     */
    bool matchMessage(const std::shared_ptr<PublishMessage>& message, MessageDomain domain,
                      mqtt::SubscriptionIds& deliverToSessions, cluster::NodeSet& deliverToBridges);

    /**
     * @brief Deliver a matched message to one shard of the session set.
     *
     * Processes only the sessions whose shardOf() equals shardIndex, so a large fan-out can be
     * split across the delivery thread pool. shardCount of 1 delivers to every session.
     * @return Number of sessions delivered to.
     */
    static size_t deliverShard(const std::shared_ptr<PublishMessage>& message, const mqtt::SubscriptionIds& deliverToSessions,
                               size_t shardIndex, size_t shardCount);

    /**
     * @brief Stable session-to-shard mapping used by deliverShard().
     */
    static size_t shardOf(const ISubscriptionClient* session, const size_t shardCount)
    {
        return std::hash<const ISubscriptionClient*> {}(session) % shardCount;
    }
    std::shared_ptr<Subscription> subscribe(const Topic* topic, const std::shared_ptr<ISubscriptionClient>& clientSession, Qos qos, uint32_t subscriptionId, SubscriptionOptions options);

    /**
     * @brief Retained messages, held per topic rather than per subscription.
     */
    RetainedMessages& retainedMessages()
    {
        return m_retainedMessages;
    }

    const RetainedMessages& retainedMessages() const
    {
        return m_retainedMessages;
    }

    void                          unsubscribe(const Topic* topic, ClientSession* clientSession);
    void                          clear();

    std::map<const Topic*, std::weak_ptr<Subscription>, std::less<>> getSubscriptions(const std::string& topicName);

private:
    mutable std::mutex                    m_mutex;
    STopicManager                         m_topicManager;
    sptk::Logger                          m_logger;
    Subscriptions                         m_topics;
    Subscriptions                         m_wildcards;
    XMQ_MAP_TYPE<RecordId, SSubscription> m_subscriptionIndex;
    RetainedMessages                      m_retainedMessages;
    Server*                               m_server;
};

using SSubscriptionManager = std::shared_ptr<SubscriptionManager>;

} // namespace xmq
