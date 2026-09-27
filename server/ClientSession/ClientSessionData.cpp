/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review          2026-02-07                                             ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "ClientSession.h"
#include "Server.h"
#include "base/ProtocolException.h"
#include "common/mqtt/PublishMessage.h"

#include <ranges>
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

ClientSessionData::ClientSessionData(Server*                   server,
                                     SConnectMessageParameters connectMessageParameters,
                                     SMessageProperties        connectMessageProperties)
    : BaseClientSession(SessionType::Server)
    , m_xmqServer(server)
    , m_connectProperties(std::move(connectMessageProperties))
    , m_nodeName(SharedStringTable::instance().intern(server->getNodeName()))
{
    applyConnectParametersUnlocked(connectMessageParameters);
}

ClientSessionData::~ClientSessionData() noexcept
{
    const unique_lock lock(m_mutex);
    if (isCleanSession())
    {
        unsubscribeAllUnlocked();
        if (const auto& queue = getInflightQueueUnlocked())
        {
            queue->clear();
        }
    }
}

std::string ClientSessionData::toString() const
{
    return format("client_id: {}, node: {}", getClientId(), getNodeName());
}

void ClientSessionData::setClientId(const std::string_view clientId)
{
    const std::unique_lock lock(m_mutex);
    // Only when it actually changes. The value is the session's identity and is the same on every
    // reconnect, so skipping the assignment keeps the string that getClientId() hands out by
    // reference from being rewritten under a reader that has already been given it.
    if (m_clientId != clientId)
    {
        m_clientId = clientId;
    }
}

const string& ClientSessionData::getClientId() const
{
    return m_clientId;
}

string_view ClientSessionData::getClientIdUnlocked() const
{
    return m_clientId;
}

void ClientSessionData::applyConnectParametersUnlocked(const SConnectMessageParameters& connectMessageParameters)
{
    if (!connectMessageParameters)
    {
        return;
    }

    // The identity is copied, not referred to: callers hold a reference or a view into these while
    // a reconnect brings another CONNECT, and the values are the same across one anyway - it is
    // only the object carrying them that is replaced.
    if (const auto& clientId = connectMessageParameters->getClientId();
        m_clientId != clientId)
    {
        m_clientId = clientId;
    }
    if (const auto username = connectMessageParameters->getUsername();
        getUsername() != username)
    {
        m_username = SharedStringTable::instance().intern(username);
    }

    m_protocolVersion.store(connectMessageParameters->m_protocolVersion, std::memory_order_relaxed);
    m_cleanSession.store(connectMessageParameters->m_cleanSession != 0, std::memory_order_relaxed);
    m_keepAliveSeconds.store(connectMessageParameters->m_keepAliveSec, std::memory_order_relaxed);
    m_lastWill.store(connectMessageParameters->m_lastWill, std::memory_order_release);

    // And the password is not taken anywhere: it is read once, on the way in, to authenticate. The
    // parameters object goes with the CONNECT message that carried it.
}

SMessageProperties ClientSessionData::getConnectProperties() const
{
    return getConnectPropertiesUnlocked();
}

SMessageProperties ClientSessionData::getConnectPropertiesUnlocked() const
{
    // By value, not by reference: a reference to the member would name a shared_ptr that a
    // reconnect reassigns, and the properties it pointed at would be released under the caller.
    return m_connectProperties.load(std::memory_order_acquire);
}

void ClientSessionData::setConnectProperties(const SMessageProperties& properties)
{
    setConnectPropertiesUnlocked(properties);
}

void ClientSessionData::setConnectPropertiesUnlocked(const SMessageProperties& properties)
{
    m_connectProperties.store(properties, std::memory_order_release);
}

const string& ClientSessionData::getNodeName() const
{
    // A reference is what the interface promises, so an unnamed session answers with something that
    // outlives the call rather than with a temporary.
    static const string none;
    return m_nodeName == nullptr ? none : *m_nodeName;
}

void ClientSessionData::setNodeName(const string& nodeName)
{
    m_nodeName = SharedStringTable::instance().intern(nodeName);
}

uint16_t ClientSessionData::getKeepAliveSeconds() const
{
    return m_keepAliveSeconds.load(std::memory_order_relaxed);
}

bool ClientSessionData::isCleanSession() const
{
    return m_cleanSession.load(std::memory_order_relaxed);
}

void ClientSessionData::setConnectMessageReceived(const bool received)
{
    m_connectMessageReceived = received;
}

bool ClientSessionData::connectMessageReceived() const
{
    return m_connectMessageReceived;
}

uint32_t ClientSessionData::getSubscriptionId(const SubscribeMessage* subscriptionMessage) const
{
    uint32_t subscriptionId = 0;
    if (const auto& subscriptionIds = subscriptionMessage->getProperties()->getSubscriptionIds();
        !subscriptionIds.empty())
    {
        subscriptionId = *subscriptionIds.begin();
        if (subscriptionId == 0)
        {
            throw ProtocolException(getProtocolVersion(), ReasonCode::ProtocolError, "Invalid subscription ID");
        }
    }
    return subscriptionId;
}

void ClientSessionData::subscribedTo(const shared_ptr<Subscription>& subscription, Qos qos, SubscriptionOptions subscriptionOptions)
{
    const std::unique_lock lock(m_mutex);
    m_subscribedTo.try_emplace(subscription->name(), subscription, qos, subscriptionOptions);
}

void ClientSessionData::unsubscribedFrom(const Subscription& subscription)
{
    const std::unique_lock lock(m_mutex);
    m_subscribedTo.erase(subscription.name());
}

const SubscriptionMap& ClientSessionData::getSubscribedToUnlocked() const
{
    return m_subscribedTo;
}

void ClientSessionData::unsubscribeAllUnlocked()
{
    auto* clientSession = dynamic_cast<ClientSession*>(this);
    for (const auto& clientSubscription: views::values(m_subscribedTo))
    {
        clientSubscription.subscription->removeSubscriptionClient(clientSession);
    }
    m_subscribedTo.clear();
}

void ClientSessionData::unsubscribeAll()
{
    const std::unique_lock lock(m_mutex);
    unsubscribeAllUnlocked();
}

void ClientSessionData::unsubscribe(const std::string_view topic)
{
    const std::unique_lock lock(m_mutex);
    auto*                  clientSession = dynamic_cast<ClientSession*>(this);
    if (const auto node = m_subscribedTo.extract(topic))
    {
        node.mapped().subscription->removeSubscriptionClient(clientSession);
    }
}

bool ClientSessionData::isSubscribed(const std::string_view topic) const
{
    const shared_lock lock(m_mutex);
    return m_subscribedTo.contains(topic);
}

void ClientSessionData::discardLastWill()
{
    m_lastWill.store(nullptr, std::memory_order_release);
}

shared_ptr<PublishMessage> ClientSessionData::getLastWillMessage(const STopicManager& topicManager) const
{
    const shared_lock lock(m_mutex);

    const auto lastWill = m_lastWill.load(std::memory_order_acquire);
    if (!lastWill)
    {
        return {};
    }

    auto lwtMessage(make_shared<mqtt::PublishMessage>(
        topicManager->getTopic(lastWill->m_topic),
        string_view(lastWill->m_message), static_cast<MessageId>(0),
        lastWill->m_retain));

    lwtMessage->setQos(lastWill->m_qos);

    return lwtMessage;
}
