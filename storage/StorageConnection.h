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

#include "base/xmq.h"

#include <sptk5/db/AutoDatabaseConnection.h>
#include <sptk5/net/RedisConnect.h>

#ifdef max
#undef max
#endif

#include "RecordId.h"
#include <sptk5/cutils>

namespace xmq {

using SDatabase = std::shared_ptr<sptk::DatabaseConnectionPool>;

/**
 * @brief Persistent storage.
 */
class XMQ_EXPORT StorageConnection
{
public:
    StorageConnection() = default;

    StorageConnection(const StorageConnection&) = delete;

    StorageConnection(StorageConnection&&) = delete;

    StorageConnection& operator=(const StorageConnection&) = delete;

    StorageConnection& operator=(StorageConnection&&) = delete;

    virtual ~StorageConnection() = default;

    /**
     * @brief Connect to storage.
     * @param database     Database URI.
     * @param redis
     */
    virtual void connect(const SDatabase& database, const sptk::SRedisConnect& redis) = 0;

    /**
     * @brief Disconnect storage (close the database).
     */
    virtual void disconnect() = 0;

    /**
     * @brief Initialize storage.
     * @param redis             Redis connection.
     * @param reset             If true, then drop and re-create the existing database.
     */
    virtual void initialize(const sptk::SRedisConnect& redis, bool reset) = 0;

    /**
     * @brief Check if storage is persistent.
     * @return True if persistent.
     */
    [[nodiscard]] virtual bool persistent() = 0;

    [[nodiscard]] virtual sptk::DateTime getCurrentTime() const = 0;

    /**
     * @brief Storage factory.
     * @param redis             Redis connection.
     * @param reset             If true, then drop and re-create the existing database.
     * @return storage.
     */
    [[nodiscard]] static std::shared_ptr<StorageConnection> create(const sptk::SRedisConnect& redis, bool reset);

    std::atomic_bool m_closing {false};
};

/**
 * @brief Shared pointer to persistent storage.
 */
using SStorageConnection = std::shared_ptr<StorageConnection>;

} // namespace xmq
