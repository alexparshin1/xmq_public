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

#include <atomic>
#include <cstdint>

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
     * @param inlineSend Write each delivery to the recipient's socket on this thread: the sender
     *                   says the fan-out is small enough for the write to be done here, and its
     *                   path asks the sessions for it (see base/MessageDispatch on what it turns
     *                   into). The deliveries that do not fit the shape ignore it.
     * @return Number of sessions delivered to.
     */
    static size_t deliverShard(const std::shared_ptr<PublishMessage>& message, const mqtt::SubscriptionIds& deliverToSessions,
                               size_t shardIndex, size_t shardCount, bool inlineSend = false);

    /**
     * @brief Stable session-to-shard mapping used by deliverShard().
     *
     * The address is mixed before it is folded. Folding it directly was measured to put every
     * subscriber of a fan-out in one shard: sessions come from one allocator size class, their
     * addresses differ by a stride that is a multiple of the page and of the alignments the shard
     * counts divide, and so all of them gave the same remainder. The whole fan-out then ran on
     * one thread however many delivery threads existed - a hundredth of the point of sharding it.
     */
    static size_t shardOf(const void* session, const size_t shardCount)
    {
        auto value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(session));
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        value *= 0xc4ceb9fe1a85ec53ULL;
        value ^= value >> 33;
        return static_cast<size_t>(value % shardCount);
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

    /**
     * @brief Add what a moving session needs: on the node it moved to, the sessions a forwarded
     *        message names; on the node it left, the node it went to, naming it.
     */
    void matchMovedSessions(const std::shared_ptr<PublishMessage>& message, mqtt::SubscriptionIds& deliverToSessions) const;

    /**
     * @brief Offer QoS 1 messages a session left unacknowledged when it ended to other members of
     *        the shared subscriptions that chose it - MQTT 5, section 4.8.2.
     * @param messages          Each message, with the shared subscription that chose the session.
     * @param endedSession      The session that ended; it is not chosen again.
     */
    void reassignShared(const std::vector<std::pair<std::weak_ptr<Subscription>, SMessage>>& messages,
                        const ISubscriptionClient* endedSession);

    /**
     * @brief This node is stopping: offer QoS 1 messages its members have not acknowledged to the
     *        members on other nodes, before the cluster links close.
     * @param messages          Each message, with the shared subscription that chose the session.
     */
    void handOffSharedToOtherNodes(const std::vector<std::pair<std::weak_ptr<Subscription>, SMessage>>& messages);

    /**
     * @brief Move the broker's retained-message counter by a change.
     * @param change            What changed.
     */
    void countRetainedChange(RetainedMessages::Change change) const;

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
