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

#include "../Cluster/ServerNode.h"
#include "SessionSubscription.h"
#include "SessionSubscriptions.h"
#include "base/Topic.h"
#include "common/ISubscriptionClient.h"
#include "common/PublishMessage.h"
#include "common/SubscriptionOptions.h"
#include "common/mqtt/SubscriptionIds.h"
#include "storage/PersistentObject.h"

#include <map>

namespace xmq {

class Subscription;

enum class MessageDomain
{
    Client,
    Server
};

using SubscriptionGroup = std::map<const Topic*, std::shared_ptr<Subscription>, std::less<>>;

namespace cluster {
class ServerNode;
using SNode = std::shared_ptr<ServerNode>;
using NodeSet = std::set<SNode>;
} // namespace cluster

/**
 * @brief Subscription.
 * @details Represents the subscription that includes the topic and subscribed clients.
 */
class XMQ_EXPORT Subscription final : public std::enable_shared_from_this<Subscription>
{
    friend class SubscriptionManager;

public:
    /**
     * @brief Private constructor.
     * @remark Use create() factory to create persistent subscriptions.
     * @param server            Server.
     * @param topic             Topic.
     * @param subscriptionGroup Parent subscription group.
     */
    Subscription(Server* server, const Topic* topic, SubscriptionGroup& subscriptionGroup);

    /**
     * @brief Subscription factory.
     * @param server            Server.
     * @param topic             Topic.
     * @param subscriptionGroup Subscription group.
     * @return Subscription.
     */
    static std::shared_ptr<Subscription> create(Server* server, const Topic* topic, SubscriptionGroup& subscriptionGroup);

    Subscription(const Subscription&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    /**
     * @brief Destructor.
     */
    ~Subscription();

    /**
     * @brief Remove clients from subscription.
     */
    void removeSubscriptionClients();

    /**
     * @brief Add the client session subscription.
     * @param subscriptionClient            Client session.
     * @param qos                           QoS.
     * @param subscriptionId                Subscription id.
     * @param subscriptionOptions           Subscription options.
     */
    void addSubscriptionClient(const std::shared_ptr<ISubscriptionClient>& subscriptionClient, Qos qos, uint32_t subscriptionId,
                               SubscriptionOptions                         subscriptionOptions);

    /**
     * @brief Add a client subscription.
     * @param sessionSubscription  Link of connection and subscription.
     * @param subscriptionClient   Subscription client.
     */
    void addSubscriptionClient(const SSessionSubscription& sessionSubscription, const std::shared_ptr<ISubscriptionClient>& subscriptionClient);

    /**
     * @brief Add bridge subscription.
     * @param clusterNode  Bridge connection.
     */
    void addClusterNode(const std::shared_ptr<cluster::ServerNode>& clusterNode);

    /**
     * @brief Remove the client session.
     * @param subscriptionClient Client session.
     */
    void removeSubscriptionClient(ISubscriptionClient* subscriptionClient);

    /**
     * @brief Remove the bridge connection.
     * @param clusterNode  Bridge connection.
     */
    void removeClusterNode(const std::shared_ptr<cluster::ServerNode>& clusterNode);

    /**
     * @brief Get the subscribed client sessions.
     * @param matchClientSessions   Optional regular expression to filter the client sessions by their client id.
     * @return List of subscribed client sessions.
     */
    [[nodiscard]] std::vector<std::shared_ptr<ISubscriptionClient>> getClientSessions(const std::string& matchClientSessions = "") const;

    /**
     * @brief Get the subscribed cluster nodes.
     * @return list of the cluster nodes.
     */
    [[nodiscard]] std::vector<std::shared_ptr<cluster::ServerNode>> getClusterNodes() const;

    /**
     * @brief Deliver the message to the subscribed clients.
     * @param publishMessage        Publish message.
     * @param deliverToSessions     List of sessions to deliver to.
     * @param deliverToClusterNodes List of cluster nodes to deliver to.
     * @param dynamicRoute          Out: set true if the resolved route depends on per-message
     *                              state (shared or no-local subscriptions) and must not be cached.
     * @param domain                Message domain: client or server.
     */
    void matchSessionsForDelivery(const PublishMessage&  publishMessage,
                                  mqtt::SubscriptionIds& deliverToSessions,
                                  cluster::NodeSet&      deliverToClusterNodes,
                                  bool&                  dynamicRoute,
                                  MessageDomain          domain);

    /**
     * @brief Subscribed clients count.
     * @return Subscribed clients count.
     */
    size_t clientCount() const;

    /**
     * @brief Does the subscription contain any clients?
     * @return True if the subscription contains any clients.
     */
    bool empty() const;

    /**
     * @brief Get the connection subscription.
     * @param subscriptionClient     Client session.
     * @return connection subscription, or nullptr if not found.
     */
    SessionSubscription* sessionSubscription(const std::shared_ptr<ISubscriptionClient>& subscriptionClient) const;

    SubscriptionGroup& subscriptionGroup() const
    {
        return *m_subscriptionGroup;
    }

    /**
     * @brief Return string presentation of subscription.
     */
    [[nodiscard]] sptk::String toString() const;

    /**
     * @brief Return topic name.
     */
    [[nodiscard]] std::string_view name() const
    {
        return m_topic->name();
    }

    /**
     * @brief Return topic name.
     */
    [[nodiscard]] std::string_view fullName() const
    {
        return m_topic->toString();
    }

    /**
     * @brief Get the topic.
     * @return topic.
     */
    const Topic* getTopic() const
    {
        return m_topic;
    }

    const Server* server() const
    {
        return m_server;
    }

    /**
     * @brief Set the topic.
     * @param topic             Topic.
     */
    void setTopic(const Topic* topic)
    {
        m_topic = topic;
    }

    /**
     * @brief Return the shared subscription flag.
     */
    [[nodiscard]] bool isShared() const
    {
        return m_topic->isShared();
    }

    /**
     * @brief Return the wildcard subscription flag.
     */
    [[nodiscard]] bool isWildcard() const
    {
        return m_topic->isWildcard();
    }

    void clearTopic();

private:
    mutable std::shared_mutex   m_mutex;                       ///< Mutex that protects internal data
    const Topic*                m_topic;                       ///< Subscription topic
    SessionSubscriptions        m_clients;                     ///< Subscribed clients
    cluster::NodeSet            m_clusterNodes;                ///< Subscribed bridges
    SubscriptionGroup*          m_subscriptionGroup {nullptr}; ///< Subscription group
    Server*                     m_server {nullptr};            ///< Server
    static std::atomic_uint32_t m_serial;                      ///< Serial number for subscription.

    /**
     * @brief Store subscription to the database.
     * @param sqlDbConnection   Database connection.
     * @param subscription      Subscription to store.
     * @param topic             Topic.
     * @param retainedMessage   Retained message.
     */
    // static void storeSubscription(const SSqlDbConnection& sqlDbConnection, const std::shared_ptr<Subscription>& subscription, const Topic* topic, std::string_view retainedMessage);
};

using SSubscription = std::shared_ptr<Subscription>;

} // namespace xmq
