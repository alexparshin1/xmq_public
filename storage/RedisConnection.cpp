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

#include "RedisConnection.h"

#include <sptk5/net/URL.h>

using namespace std;
using namespace sptk;
using namespace xmq;

/**
 * @brief Naming conventions:
 * node_[name] - contains [node settings].
 * node_[name]_sessions - hash that contains sessions: {session_[client_id], session JSON}.
 * session_[client_id]_messages - hash that contains messages: {[message serial], [message]}.
 */
RedisConnection::RedisConnection()
    : m_redis(make_shared<RedisConnect>())
{
}

void RedisConnection::connect(const SDatabase&, const SRedisConnect&)
{
}

void RedisConnection::disconnect()
{
}

void RedisConnection::initialize(const SRedisConnect& redis, const bool reset)
{
    const URL redisUrl(redis->toString());
    m_redis->connect(redisUrl);
    // A clean start is scoped to the owning node, so it needs a node name this method does
    // not have. Storage::initialize() performs it via clearNodeState() before creating
    // this connection; there is deliberately nothing to do here.
    (void) reset;
}

void RedisConnection::clearNodeState(const SRedisConnect& redis, const std::string& nodeName)
{
    const auto nodeSessionsKey = "node_" + nodeName + "_sessions";

    // Delete in batches: one command per session costs a round trip each, and a single
    // command naming a million keys is its own problem.
    constexpr size_t         batchSize = 1000;
    std::vector<std::string> keys;
    keys.reserve(batchSize * 2 + 1);

    for (const auto& clientId: redis->getSetMembers(nodeSessionsKey))
    {
        keys.push_back("session_" + clientId);
        keys.push_back("session_" + clientId + "_messages");
        if (keys.size() >= batchSize * 2)
        {
            (void) redis->deleteKeys(keys);
            keys.clear();
        }
    }

    keys.push_back(nodeSessionsKey);

    // Retained messages belong to this node's persisted state as much as its sessions do; the key
    // is built by RetainedMessages::storageKey(), kept in step by hand rather than by including
    // the server's headers into storage.
    keys.push_back("node_" + nodeName + "_retained");

    (void) redis->deleteKeys(keys);
}

bool RedisConnection::persistent()
{
    return true;
}

DateTime RedisConnection::readServerTime(const SRedisConnect& redis)
{
    if (!redis || !redis->isConnected())
    {
        return DateTime();
    }

    try
    {
        const RedisCommand command("TIME");
        vector<Variant>    results;
        redis->executeCommand(command, results);

        // TIME answers with two values: whole seconds since the epoch, and microseconds elapsed
        // within that second.
        constexpr size_t expectedResultCount = 2;
        if (results.size() < expectedResultCount)
        {
            return DateTime();
        }

        const auto sinceEpoch = chrono::seconds(results[0].asInt64()) + chrono::microseconds(results[1].asInt64());

        return DateTime(chrono::duration_cast<DateTime::duration>(sinceEpoch));
    }
    catch (const Exception&)
    {
        // Callers treat an epoch result as "unavailable" and keep using what they had.
        return DateTime();
    }
}

DateTime RedisConnection::getCurrentTime() const
{
    return readServerTime(m_redis);
}
