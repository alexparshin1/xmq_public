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
#include <sptk5/net/RedisConnect.h>

#ifdef max
#undef max
#endif

#include "StorageConnection.h"
#include <sptk5/cutils>

namespace xmq {

using RecordId = int64_t;

class Server;

/**
 * @brief Persistent storage.
 */
class XMQ_EXPORT Storage final
{
public:
    /**
     * @brief Process function called for each persistent object.
     */
    using ProcessFunction = std::function<void(RecordId recordId, const sptk::FieldList& data)>;

    explicit Storage(Server* server)
        : m_server(server)
    {
    }

    Storage(const Storage&) = delete;

    Storage(Storage&&) = delete;

    Storage& operator=(const Storage&) = delete;

    Storage& operator=(Storage&&) = delete;

    ~Storage();

    /**
     * Connect to database.
     * @param redisConnectString
     */
    void connect(const std::string& redisConnectString);

    /**
     * @brief Disconnect storage (close database).
     */
    void disconnect();

    /**
     * @brief Initialize storage.
     * @param redisConnectString
     * @param redisConnectString
     * @param reset             If true, then drop and re-create the existing database.
     */
    void initialize(const std::string& redisConnectString, bool reset);

    [[nodiscard]] sptk::String toString() const;

    /**
     * @brief Check if storage is persistent.
     * @return True if persistent.
     */
    [[nodiscard]] bool isPersistent() const
    {
        return m_persistent.load();
    }

    [[nodiscard]] Server* server() const
    {
        return m_server;
    }

    /**
     * @brief Get the current time on the storage backend's clock.
     *
     * Anchored to the backend rather than the host, so that every node in a cluster compares
     * timestamps against the same reference regardless of how far its own clock has drifted.
     * The backend is not queried on every call: an offset from the host clock is sampled
     * periodically and applied locally, so this stays cheap enough to call freely.
     *
     * @return Current storage time, or host time while the backend is unreachable.
     */
    [[nodiscard]] sptk::DateTime getCurrentTime() const;

    /**
     * @brief Storage factory.
     * @param server            Server.
     * @param redisConnectString       Redis connection string.
     * @param reset             If true, then drop and re-create the existing database.
     * @return storage.
     */
    static std::shared_ptr<Storage> create(Server* server, const std::string& redisConnectString, bool reset);

    void logError(const std::string& message) const;

    sptk::SRedisConnect getRedis() const
    {
        return m_redis;
    }

private:
    /**
     * @brief Offset from the host clock to the storage clock, re-sampled when stale.
     * @return The offset to add to host time.
     */
    [[nodiscard]] std::chrono::milliseconds storageClockOffset() const;

    Server*             m_server;
    sptk::SRedisConnect m_redis {std::make_shared<sptk::RedisConnect>()};
    std::atomic_bool    m_persistent {false};

    mutable std::mutex                m_clockMutex;      ///< Guards the sampled clock offset.
    mutable std::chrono::milliseconds m_clockOffset {0}; ///< Host-to-storage clock offset.
    mutable sptk::DateTime            m_clockSampledAt;  ///< Host time the offset was last sampled, epoch if never.
};

/**
 * @brief Shared pointer to persistent storage.
 */
using SStorage = std::shared_ptr<Storage>;

} // namespace xmq
