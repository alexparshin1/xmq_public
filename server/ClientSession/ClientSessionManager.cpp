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

#include "ClientSessionManager.h"
#include "common/mqtt/PublishMessage.h"
#include "server/Server.h"

#include "base/DescriptorLimit.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

ClientSessionManager::ClientSessionManager(Server* server)
    : m_xmqServer(server)
{
    const auto reservation = possibleConnectionCount();
    m_clientConnectionsByName.reserve(reservation);
    m_clientConnectionsByAddress.reserve(reservation);
}

void ClientSessionManager::add(const SClientSession& clientSession)
{
    const unique_lock lock(m_mutex);

    if (const string clientId(clientSession->getClientId());
        !clientId.empty())
    {
        m_clientConnectionsByName[clientId] = clientSession;
        m_clientConnectionsByAddress.erase(clientSession.get());
    }
    else
    {
        m_clientConnectionsByAddress[clientSession.get()] = clientSession;
    }
}

SClientSession ClientSessionManager::find(const string& clientId)
{
    const shared_lock lock(m_mutex);
    const auto        iterator = m_clientConnectionsByName.find(clientId);
    if (iterator == m_clientConnectionsByName.end())
    {
        return nullptr;
    }
    return iterator->second;
}

void ClientSessionManager::clear()
{
    const unique_lock lock(m_mutex);
    m_clientConnectionsByName.clear();
    m_clientConnectionsByAddress.clear();
}

vector<CConnectionInfo> ClientSessionManager::getClientConnectionsInfo(const RegularExpression& matchClientName)
{
    const shared_lock lock(m_mutex);

    vector<CConnectionInfo> result;

    for (const auto& client: views::values(m_clientConnectionsByName))
    {
        if (matchClientName.matches(client->getClientId()))
        {
            CConnectionInfo reportClient;
            reportClient.m_client_name = client->getClientId();
            reportClient.m_is_connected = client->isConnected();
            result.push_back(std::move(reportClient));
        }
    }

    return result;
}

void ClientSessionManager::load(const SStorage& storage, const string& nodeId)
{
    clear();

    if (!storage || !storage->isPersistent())
    {
        return;
    }

    const auto redis = storage->getRedis();

    for (const auto  clientIds = redis->getSetMembers(format("node_{}_sessions", nodeId));
         const auto& clientId: clientIds)
    {
        const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
        connectMessageParameters->m_cleanSession = false;
        connectMessageParameters->setClientId(clientId);
        auto clientSession = ClientSession::factory(getServer(), connectMessageParameters);
        add(clientSession);
    }
}

size_t ClientSessionManager::clientCount() const
{
    const shared_lock lock(m_mutex);
    return m_clientConnectionsByName.size();
}

void ClientSessionManager::remove(const SClientSession& clientSession)
{
    const unique_lock lock(m_mutex);
    removeUnlocked(clientSession);
}

void ClientSessionManager::eraseIfStillOurs(const string& clientId, const ClientSession* clientSession)
{
    // A client id names whichever session holds it now, and that is not always the one being
    // removed. A reconnect on the same id registers its session under that name while the old
    // one is still being torn down - the old one's removal arrives second and, erasing by name
    // alone, took the newcomer's entry with it.
    //
    // The broker then had no session for a client it had just sent a CONNACK to. The client
    // believed itself connected, and its SUBSCRIBE went unanswered - the shape a bridge shows
    // when it reconnects and then carries nothing.
    if (const auto found = m_clientConnectionsByName.find(clientId);
        found != m_clientConnectionsByName.end() && found->second.get() == clientSession)
    {
        m_clientConnectionsByName.erase(found);
    }
}

void ClientSessionManager::removeUnlocked(const SClientSession& clientSession)
{
    // Copy before erasing: clientSession may alias a map slot destroyed by the first erase.
    const auto*  address = clientSession.get();
    const string clientId(clientSession->getClientId());

    if (!clientId.empty())
    {
        eraseIfStillOurs(clientId, address);
    }
    m_clientConnectionsByAddress.erase(address);
}

void ClientSessionManager::removeUnlocked(const string& clientId)
{
    const auto iterator = m_clientConnectionsByName.find(clientId);
    if (iterator == m_clientConnectionsByName.end())
    {
        return;
    }
    const SClientSession clientSession = iterator->second;
    m_clientConnectionsByName.erase(iterator);
    m_clientConnectionsByAddress.erase(clientSession.get());
}

void ClientSessionManager::remove(const ClientSession* clientSession)
{
    const unique_lock lock(m_mutex);
    if (clientSession)
    {
        if (const string clientId(clientSession->getClientId());
            !clientId.empty())
        {
            eraseIfStillOurs(clientId, clientSession);
        }
        m_clientConnectionsByAddress.erase(clientSession);
    }
}

void ClientSessionManager::forEach(const function<void(const SClientSession&)>& action)
{
    const unique_lock lock(m_mutex);
    for (const auto& clientSession: views::values(m_clientConnectionsByName))
    {
        action(clientSession);
    }
}
