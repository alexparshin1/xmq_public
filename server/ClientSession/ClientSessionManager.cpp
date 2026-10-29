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

vector<SClientSession> ClientSessionManager::findSessions(const string_view prefix, const size_t limit) const
{
    const shared_lock lock(m_mutex);
    vector<SClientSession> result;
    result.reserve(limit + 1);
    for (auto it = m_clientConnectionsByName.lower_bound(prefix); it != m_clientConnectionsByName.end(); ++it)
    {
        if (!it->first.starts_with(prefix))
        {
            break;
        }
        result.push_back(it->second);
        if (result.size() > limit)
        {
            break;
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
    const auto clientIds = redis->getSetMembers(format("node_{}_sessions", nodeId));

    // Restoring a session used to mean three synchronous round trips - the session record, its
    // message hash and its unreleased QoS 2 ids - one after another. At 60,000 sessions on a
    // 10GbE link that came to 180,000 waits, 31.7 seconds, while Redis itself spent 0.2 seconds
    // answering them. The batch below asks for the same data for a few thousand sessions at a
    // time: one MGET for the records, and the hash reads queued so the connection pipelines
    // them, with a single wait per batch.
    constexpr size_t batchSize = 2048;

    for (size_t start = 0; start < clientIds.size(); start += batchSize)
    {
        const auto count = min(batchSize, clientIds.size() - start);

        vector<string> sessionKeys;
        sessionKeys.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            sessionKeys.emplace_back("session_" + clientIds[start + i]);
        }

        const auto records = redis->getValues(sessionKeys);

        vector<RedisConnect::KeysAndValues> messages(count);
        vector<vector<string>>              qos2Keys(count);
        vector<char>                        fetched(count, 0);

        for (size_t i = 0; i < count; ++i)
        {
            // Queued, not waited for: the connection pipelines what it can, and one wait at the
            // end covers the batch. A read that fails leaves its bit clear, so that session is
            // restored by the ordinary path below rather than silently without its messages.
            const auto& clientId = clientIds[start + i];
            redis->getHashValuesAsync(format("session_{}_messages", clientId),
                                      [&messages, &fetched, i](const RedisConnect::KeysAndValues& values)
                                      {
                                          messages[i] = values;
                                          fetched[i] |= 1;
                                      });
            redis->getHashKeysAsync(format("session_{}_qos2", clientId),
                                    [&qos2Keys, &fetched, i](const vector<string>& keys)
                                    {
                                        qos2Keys[i] = keys;
                                        fetched[i] |= 2;
                                    });
        }
        redis->waitForAsyncCompletion();

        for (size_t i = 0; i < count; ++i)
        {
            const auto& clientId = clientIds[start + i];
            auto        connectMessageParameters = make_shared<ConnectMessageParameters>();
            connectMessageParameters->m_cleanSession = false;
            connectMessageParameters->setClientId(clientId);

            if (const auto record = records.find(sessionKeys[i]);
                fetched[i] == 3 && record != records.end() && !record->second.isNull())
            {
                add(ClientSession::factory(getServer(), connectMessageParameters, {}, record->second,
                                           std::move(messages[i]), std::move(qos2Keys[i])));
            }
            else
            {
                add(ClientSession::factory(getServer(), connectMessageParameters));
            }
        }
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
