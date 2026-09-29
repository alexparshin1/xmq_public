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

#include "RedisStorage.h"

#include "MessageDeliveryPacker.h"
#include "RedisConnection.h"
#include "server/MessageDelivery.h"
#include "server/Server.h"

#include <ranges>
#include <sptk5/net/Host.h>
#include <sptk5/xdoc/Document.h>

using namespace std;
using namespace sptk;
using namespace xmq;

RedisStorage::RedisStorage(URL redisUrl, Server* server, const size_t connectionPoolSize, const bool cleanStart)
    : m_redisUrl(std::move(redisUrl))
    , m_server(server)
    , m_cleanStart(cleanStart)
    , m_cleanupConnection(make_shared<RedisConnect>())
    , m_sharedConnections(max<size_t>(connectionPoolSize, 1))
{
    // Asynchronous cleanup failures never reach the per-operation callbacks, so surface them to the
    // log via the connection's error handler.
    m_cleanupConnection->setAsyncErrorHandler(
        [server](const Exception& exception)
        {
            if (server != nullptr)
            {
                server->logMessage(LogSubject::SessionErrors, LogPriority::Error,
                                   format("Asynchronous session cleanup failed: {}", exception.what()));
            }
        });
}

RedisStorage::~RedisStorage()
{
    disconnect();
}

void RedisStorage::connect() const
{
    const scoped_lock lock(m_mutex);
    if (m_cleanStart)
    {
        clear();
    }
    m_cleanupConnection->connect(m_redisUrl);
}

void RedisStorage::disconnect() const
{
    const scoped_lock lock(m_mutex);
    for (const auto& redisConnection: m_sharedConnections)
    {
        if (redisConnection)
        {
            redisConnection->disconnect();
        }
    }
}

void RedisStorage::clear() const
{
    const auto redisConnection = make_shared<RedisConnect>();
    redisConnection->connect(m_redisUrl);

    // Clean start drops THIS node's persisted state only - never FLUSHDB. See
    // RedisConnection::clearNodeState().
    RedisConnection::clearNodeState(redisConnection, m_server->getNodeName());
}

SRedisConnect RedisStorage::getRedis()
{
    const auto index = m_nextSharedConnection++ % m_sharedConnections.size();
    const scoped_lock lock(m_mutex);
    auto& connection = m_sharedConnections[index];
    if (!connection)
    {
        connection = makeConnection();
    }
    return connection;
}

SRedisConnect RedisStorage::makeConnection() const
{
    auto redisConnect = make_shared<RedisConnect>();

    // Asynchronous writes report failures here and nowhere else - without a handler a session
    // that failed to persist would be silently lost. The connect path queues its writes rather
    // than waiting for them (see PersistentClientSession::initSession), which is what makes this
    // handler load-bearing rather than decorative.
    redisConnect->setAsyncErrorHandler(
        [server = m_server](const Exception& exception)
        {
            if (server != nullptr)
            {
                server->logMessage(LogSubject::SessionErrors, LogPriority::Error,
                                   format("Asynchronous session write failed: {}", exception.what()));
            }
        });
    redisConnect->connect(m_redisUrl);
    return redisConnect;
}

void RedisStorage::cleanupSessionAsync(const string& clientId, const string& nodeName) const
{
    // The reactor thread only queues the work: getValueAsync returns immediately and the lookup,
    // along with the conditional deletes issued from its callback, run on the connection's own
    // worker thread (pipelined). Failures don't reach these callbacks; they are reported to the
    // async error handler registered in the constructor, which logs them.
    const auto redisConnection = m_cleanupConnection;
    const auto sessionKey = "session_" + clientId;

    redisConnection->getValueAsync(
        sessionKey,
        [redisConnection, sessionKey, clientId, nodeName](const Variant& sessionInfo)
        {
            // Nothing persisted for this client: nothing to clean up.
            if (sessionInfo.isNull())
            {
                return;
            }

            xdoc::Document sessionJson;
            sessionJson.load(sessionInfo.asBuffer());
            if (sessionJson.root()->getString("node_name").empty())
            {
                return;
            }

            // Drop the session record, its queued messages and its membership in the node's session
            // set. The messages hash (see sessionMessagesHash()) would otherwise be orphaned.
            const auto sessionMessagesKey = "session_" + clientId + "_messages";
            const auto nodeSessionsKey = "node_" + nodeName + "_sessions";
            redisConnection->deleteKeysAsync({sessionKey, sessionMessagesKey}, [](size_t)
                                             {
                                             });
            redisConnection->deleteSetMembersAsync(nodeSessionsKey, {clientId}, [](size_t)
                                                   {
                                                   });
        });
}

std::string RedisStorage::sessionMessagesHash(const SClientSession& session)
{
    return format("session_{}_messages", session->getClientId());
}
