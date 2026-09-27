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
#include <sptk5/cdatabase>

namespace xmq {

class XMQ_EXPORT DummyDbConnection final : public StorageConnection
{
public:
    using StorageConnection::StorageConnection;

    DummyDbConnection(const DummyDbConnection&) = delete;
    DummyDbConnection(DummyDbConnection&&) = delete;
    DummyDbConnection& operator=(const DummyDbConnection&) = delete;
    DummyDbConnection& operator=(DummyDbConnection&&) = delete;

    ~DummyDbConnection() override
    {
        DummyDbConnection::disconnect();
    }

    /**
     * Constructor
     */
    void connect(const SDatabase&, const sptk::SRedisConnect&) override
    {
        // Do nothing
    }

    void initialize(const sptk::SRedisConnect&, bool) override
    {
        // Do nothing
    }

    /**
     * @brief Disconnect storage (close the database).
     */
    void disconnect() override
    {
        // Do nothing
    }

    [[nodiscard]] sptk::DateTime getCurrentTime() const override
    {
        return sptk::DateTime::Now();
    }

    /**
     * @brief Check if storage is persistent.
     * @return.
     */
    [[nodiscard]] bool persistent() override
    {
        return false;
    }
};

} // namespace xmq
