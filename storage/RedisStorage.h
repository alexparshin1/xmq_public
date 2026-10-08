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

#include "server/ClientSession/ClientSession.h"


#include <sptk5/net/Host.h>
#include <sptk5/net/RedisConnect.h>
#include <sptk5/net/URL.h>

namespace xmq {

class Server;
class ClientSession;
class MessageDelivery;

/**
 * @brief Redis storage.
 * @remarks Provides a Redis connection per caller thread. That is necessary to correctly handle
 * Redis transactions.
 */
class RedisStorage
{
public:
    /**
     * @brief Constructor.
     * @param redisUrl              Redis server URL, including the optional username and password.
     * @param server                XMQ server.
     * @param connectionPoolSize    Number of Redis connections in the pool.
     * @param cleanStart            If true, then clean the storage upon start.
     */
    RedisStorage(sptk::URL redisUrl, Server* server, size_t connectionPoolSize, bool cleanStart);

    /**
     * @brief Destructor. Closes the Redis connections.
     *
     * disconnect() exists for that, but nothing ever calls it, so the connections were held
     * until the process exited.
     */
    virtual ~RedisStorage();

    void connect() const;
    void disconnect() const;

    /**
     * @brief Wait until every command queued on the pool's connections so far has been answered.
     * @param timeout           How long to wait for each connection.
     * @return False if a connection did not finish in time.
     */
    [[nodiscard]] bool waitForWrites(std::chrono::milliseconds timeout) const;

    [[nodiscard]] Server* server() const
    {
        return m_server;
    }

    [[nodiscard]] sptk::SRedisConnect getRedis();

    /**
     * @brief Asynchronously remove any persisted session for @p clientId.
     *
     * A clean-session connect must not leave a stale persisted session behind (possibly created by
     * another cluster node). The lookup and deletes are Redis round-trips, so they are issued via
     * RedisConnect's asynchronous API on a dedicated connection: the connecting reactor thread only
     * queues the work and returns immediately, while the connection's own worker thread pipelines
     * the round-trips. Failures are reported to the connection's async error handler and logged.
     */
    void cleanupSessionAsync(const std::string& clientId, const std::string& nodeName) const;

    [[nodiscard]] static std::string sessionMessagesHash(const SClientSession& session);

    /**
     * @brief Redis server host and port, without the credentials.
     */
    [[nodiscard]] sptk::Host host() const
    {
        const auto& [hostname, port] = m_redisUrl.hostAndPort();
        return {hostname, port};
    }


private:
    mutable std::mutex m_mutex;
    sptk::URL          m_redisUrl; ///< Redis server URL, including the optional credentials.
    Server*            m_server {nullptr};
    bool               m_cleanStart {false}; ///< If true then clean the database upon the connecting.

    sptk::SRedisConnect m_cleanupConnection; ///< Dedicated connection for asynchronous session cleanup.

    /// persistence.max_redis_connections of them, shared round-robin rather than one per thread:
    /// fewer connections carry more commands per write, and Redis spends its time on commands
    /// rather than on reading sockets. Two measured best at 50K persistent messages/s - one was
    /// slower, and more spread the same load thinner.
    std::vector<sptk::SRedisConnect> m_sharedConnections;
    std::atomic_size_t               m_nextSharedConnection {0};

    sptk::SRedisConnect makeConnection() const;
    void clear() const;
};

using SRedisStorage = std::shared_ptr<RedisStorage>;

} // namespace xmq
