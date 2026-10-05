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

#include "Subscription.h"
#include "Server.h"
#include "Cluster/Cluster.h"
#include "SessionSubscription.h"

#include <ranges>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
bool hasEffectiveClients(const SessionSubscriptions& clients)
{
    return clients.any_of([](ISubscriptionClient* client, const SSessionSubscription&)
                          {
                              // Cluster links are not customer interest; both ends use the
                              // reserved cluster username on their subscription clients.
                              return client->getUsername() != "cluster";
                          });
}
}

atomic_uint32_t Subscription::m_serial;

std::shared_ptr<Subscription> Subscription::create(Server* server, const Topic* topic, SubscriptionGroup& subscriptionGroup)
{
    auto subscription = make_shared<Subscription>(server, topic, subscriptionGroup);
    return subscription;
}

Subscription::Subscription(Server* server, const Topic* topic, SubscriptionGroup& subscriptionGroup)
    : m_topic(topic)
      , m_subscriptionGroup(&subscriptionGroup)
      , m_server(server)
{
    ++m_serial;
}

Subscription::~Subscription()
{
    if (auto* topic = const_cast<Topic*>(m_topic))
    {
        topic->setSubscription({});
    }
}

void Subscription::addSubscriptionClient(const std::shared_ptr<ISubscriptionClient>& subscriptionClient,
                                         const Qos                                   qos,
                                         const uint32_t                              subscriptionId,
                                         const SubscriptionOptions                   subscriptionOptions)
{
    if (subscriptionClient != nullptr)
    {
        bool wasEffective = false;
        bool isEffective = false;
        {
            const unique_lock lock(m_mutex);
            wasEffective = hasEffectiveClients(m_clients);

            const SSubscription self = dynamic_pointer_cast<Subscription>(shared_from_this());
            const auto          sessionSubscription = SessionSubscription::create(subscriptionClient, self, qos, subscriptionOptions, subscriptionId);
            m_clients.addClient(subscriptionClient.get(), sessionSubscription);
            isEffective = hasEffectiveClients(m_clients);
        }
        if (wasEffective != isEffective && !m_server->isStopping())
        {
            if (const auto cluster = m_server->getCluster())
            {
                cluster->updateLocalSubscription(fullName(), isEffective);
            }
        }
    }
}

void Subscription::addSubscriptionClient(const SSessionSubscription& sessionSubscription, const std::shared_ptr<ISubscriptionClient>& subscriptionClient)
{
    if (subscriptionClient != nullptr)
    {
        bool wasEffective = false;
        bool isEffective = false;
        {
            const unique_lock lock(m_mutex);
            wasEffective = hasEffectiveClients(m_clients);
            m_clients.addClient(subscriptionClient.get(), sessionSubscription);
            isEffective = hasEffectiveClients(m_clients);
        }
        if (wasEffective != isEffective && !m_server->isStopping())
        {
            if (const auto cluster = m_server->getCluster())
            {
                cluster->updateLocalSubscription(fullName(), isEffective);
            }
        }
    }
}

void Subscription::removeSubscriptionClient(ISubscriptionClient* subscriptionClient)
{
    if (subscriptionClient)
    {
        bool wasEffective = false;
        bool isEffective = false;
        {
            const unique_lock lock(m_mutex);
            wasEffective = hasEffectiveClients(m_clients);
            m_clients.removeClient(subscriptionClient);
            isEffective = hasEffectiveClients(m_clients);
        }
        if (wasEffective != isEffective && !m_server->isStopping())
        {
            if (const auto cluster = m_server->getCluster())
            {
                cluster->updateLocalSubscription(fullName(), isEffective);
            }
        }
    }
}

void Subscription::removeSubscriptionClients()
{
    bool wasEffective;
    {
        const unique_lock lock(m_mutex);
        wasEffective = hasEffectiveClients(m_clients);
        m_clients.clear();
    }
    if (wasEffective && !m_server->isStopping())
    {
        if (const auto cluster = m_server->getCluster())
        {
            cluster->updateLocalSubscription(fullName(), false);
        }
    }
}

vector<shared_ptr<ISubscriptionClient>> Subscription::getClientSessions(const std::string& matchClientSessions) const
{
    vector<shared_ptr<ISubscriptionClient>> subscribedClients;

    const shared_lock lock(m_mutex);
    RegularExpression filter(matchClientSessions);

    m_clients.for_each(
        [&subscribedClients, &filter, &matchClientSessions](const ISubscriptionClient* client, const auto& sessionSubscription)
        {
            if (matchClientSessions.empty() || filter.m(client->getClientId()))
            {
                subscribedClients.push_back(sessionSubscription->clientSession());
            }
        });

    return subscribedClients;
}

std::vector<shared_ptr<cluster::ServerNode>> Subscription::getClusterNodes() const
{
    vector<shared_ptr<cluster::ServerNode>> clusterNodes;

    const shared_lock lock(m_mutex);

    for (const auto& clusterNode: m_clusterNodes)
    {
        clusterNodes.push_back(clusterNode);
    }

    return clusterNodes;
}

void Subscription::matchSessionsForDelivery(const PublishMessage&  publishMessage,
                                            mqtt::SubscriptionIds& deliverToSessions,
                                            cluster::NodeSet&      deliverToClusterNodes,
                                            bool&                  dynamicRoute,
                                            const MessageDomain    domain)
{
    // A shared subscription picks a different client per message (round-robin), so its route can't
    // be memorised by topic.
    if (isShared())
    {
        dynamicRoute = true;
    }

    // A retained publication with an empty payload clears the stored value, but it is still a
    // publication. In particular, cluster subscribers must receive it to clear their own copy.

    // Who may be handed this message at all. Shared and not: a shared subscription offers the
    // message to the next member that passes, rather than to the next member.
    auto eligible = [&publishMessage, &dynamicRoute](const ISubscriptionClient* clientSession, const SSessionSubscription& subscription)
    {
        if (subscription->options().m_noLocal)
        {
            // Membership depends on the publisher's client id, so this route can't be memoised.
            dynamicRoute = true;
            if (publishMessage.getSender() == clientSession->getClientId())
            {
                return false;
            }
        }
        // Not to a bridge or cluster link if the message came from another node: it goes back
        // nowhere it came from, and the node it came from has delivered it there already.
        return clientSession->bridgeOrigin().empty() || publishMessage.getSourceNode().empty();
    };

    auto addToDeliverToSessions = [&deliverToSessions](const ISubscriptionClient*, const SSessionSubscription& subscription)
    {
        const auto* clientSubscriptionDetails = subscription.get();
        deliverToSessions.add(subscription->clientSession(), clientSubscriptionDetails->qos(),
                              clientSubscriptionDetails->subscriptionId(), clientSubscriptionDetails->options());
    };

    const shared_lock lock(m_mutex);

    if (isShared())
    {
        m_clients.for_next(addToDeliverToSessions, eligible);
    }
    else
    {
        m_clients.for_each([&eligible, &addToDeliverToSessions](ISubscriptionClient* clientSession, const SSessionSubscription& subscription)
                           {
                               if (eligible(clientSession, subscription))
                               {
                                   addToDeliverToSessions(clientSession, subscription);
                               }
                           });
    }

    if (!m_clusterNodes.empty())
    {
        for (const auto& clusterNode: m_clusterNodes)
        {
            deliverToClusterNodes.insert(clusterNode);
        }
    }
}

void Subscription::clearTopic()
{
    const unique_lock lock(m_mutex);
    m_topic = nullptr;
}

bool Subscription::empty() const
{
    const shared_lock lock(m_mutex);
    return m_clients.empty();
}

size_t Subscription::clientCount() const
{
    const shared_lock lock(m_mutex);
    return m_clients.size();
}

SessionSubscription* Subscription::sessionSubscription(const shared_ptr<ISubscriptionClient>& subscriptionClient) const
{
    return m_clients.getSubscription(subscriptionClient.get()).get();
}

String Subscription::toString() const
{
    std::stringstream output;

    output << "path='" << name() << "'";

    return output.str();
}

void Subscription::addClusterNode(const std::shared_ptr<cluster::ServerNode>& clusterNode)
{
    const unique_lock lock(m_mutex);
    m_clusterNodes.insert(clusterNode);
}

void Subscription::removeClusterNode(const shared_ptr<cluster::ServerNode>& clusterNode)
{
    const unique_lock lock(m_mutex);
    m_clusterNodes.erase(clusterNode);
}
