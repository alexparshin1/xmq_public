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
#include "ClientSession.h"
#include "Server.h"
#include "SessionSubscription.h"

#include <ranges>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

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
        const unique_lock lock(m_mutex);

        const SSubscription self = dynamic_pointer_cast<Subscription>(shared_from_this());
        const auto          sessionSubscription = SessionSubscription::create(subscriptionClient, self, qos, subscriptionOptions, subscriptionId);
        m_clients.addClient(subscriptionClient.get(), sessionSubscription);
    }
}

void Subscription::addSubscriptionClient(const SSessionSubscription& sessionSubscription, const std::shared_ptr<ISubscriptionClient>& subscriptionClient)
{
    if (subscriptionClient != nullptr)
    {
        const unique_lock lock(m_mutex);
        m_clients.addClient(subscriptionClient.get(), sessionSubscription);
    }
}

void Subscription::removeSubscriptionClient(ISubscriptionClient* subscriptionClient)
{
    if (subscriptionClient)
    {
        m_clients.removeClient(subscriptionClient);
    }
}

void Subscription::removeSubscriptionClients()
{
    const unique_lock lock(m_mutex);
    m_clients.clear();
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

    auto addToDeliverToSessions = [&publishMessage, &deliverToSessions, &dynamicRoute](const ISubscriptionClient* clientSession, const SSessionSubscription& subscription)
    {
        const auto* clientSubscriptionDetails = subscription.get();
        const auto  options = clientSubscriptionDetails->options();
        if (options.m_noLocal)
        {
            // Membership depends on the publisher's client id, so this route can't be memoised.
            dynamicRoute = true;
            if (publishMessage.getSender() == clientSession->getClientId())
            {
                return;
            }
        }
        if (!clientSession->bridgeOrigin().empty() && !publishMessage.getSourceNode().empty())
        {
            // Don't deliver to bridge subscription if the message came from another node.
            return;
        }

        deliverToSessions.add(subscription->clientSession(), clientSubscriptionDetails->qos(), clientSubscriptionDetails->subscriptionId(), options);
    };

    const shared_lock lock(m_mutex);

    if (isShared())
    {
        m_clients.for_next(addToDeliverToSessions);
    }
    else
    {
        m_clients.for_each(addToDeliverToSessions);
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
