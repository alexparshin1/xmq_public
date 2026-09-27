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

#include "StorageConnection.h"

namespace xmq {

class RedisConnection
    : public StorageConnection
{
public:
    /**
     * @brief Constructor.
     */
    RedisConnection();

    RedisConnection(const RedisConnection&) = delete;

    RedisConnection(RedisConnection&&) = delete;

    RedisConnection& operator=(const RedisConnection&) = delete;

    RedisConnection& operator=(RedisConnection&&) = delete;

    /**
     * @brief Destructor.
     */
    ~RedisConnection() override = default;

    void                               connect(const SDatabase& database, const sptk::SRedisConnect& redis) override;
    void                               disconnect() override;
    void                               initialize(const sptk::SRedisConnect& redis, bool reset) override;

    /**
     * @brief Drop one node's persisted state: its sessions, their queued messages, and its
     * session set.
     *
     * This is what "clean start" means. It deliberately does NOT use FLUSHDB: that empties
     * the whole database, so on a Redis shared with other XMQ nodes - or with anything
     * else - one node starting clean silently destroyed everyone else's sessions.
     *
     * @param redis             Redis connection.
     * @param nodeName          Name of the node whose state is dropped.
     */
    static void clearNodeState(const sptk::SRedisConnect& redis, const std::string& nodeName);

    /**
     * @brief Read the current time from the Redis server.
     *
     * Liveness decisions that several nodes have to agree on cannot be made against host clocks,
     * which drift apart independently. Reading the time from the shared backend gives every node
     * the same reference, whatever its own clock says.
     *
     * @param redis             Redis connection.
     * @return The server's current time, or an epoch DateTime if it could not be read.
     */
    [[nodiscard]] static sptk::DateTime readServerTime(const sptk::SRedisConnect& redis);
    [[nodiscard]] bool                 persistent() override;
    [[nodiscard]] sptk::DateTime       getCurrentTime() const override;
    [[nodiscard]] sptk::SRedisConnect& getRedis()
    {
        return m_redis;
    }

private:
    sptk::SRedisConnect m_redis;
};

} // namespace xmq
