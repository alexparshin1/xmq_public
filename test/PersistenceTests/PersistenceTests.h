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

#include "TestServers.h"
#include "test/ServerTests/ServerTests.h"

namespace xmq {

class XMQ_EXPORT XMQ_PersistenceTests : public XMQ_ServerTests
{
public:
    void SetUp() override;

    sptk::String m_databaseUri{TestServers::dbConnectString(TestServers::StorageType::PostgreSQL)};

    static SClientSession createClientSession(const std::string& clientId, const std::vector<std::string>& topicNames = {}, const std::vector<std::string>& messagePayloadsPerTopic = {});

    static bool areClientSessionMessagesInRedis(const SClientSession& clientSession, size_t expectedMessageCount);
    static bool isClientSessionInRedis(const SClientSession& clientSession);
    static bool isClientSessionInRedis(const SClientSession& clientSession, size_t expectedMessageCount);

    /**
     * @brief Assert the session, its node membership, and its queued messages are all in Redis.
     * @param redis                 Redis connection.
     * @param clientId              Session client id.
     * @param expectedMessageCount  Expected number of queued messages.
     */
    static void expectSessionInRedis(const sptk::SRedisConnect& redis, const std::string& clientId, size_t expectedMessageCount);

    /**
     * @brief Assert the server holds a session with the given subscriptions and queued messages.
     *
     * Call before reconnecting any client - that is what distinguishes state restored at
     * startup from state loaded on connect.
     *
     * @param clientId              Session client id.
     * @param topicNames            Topics the session is expected to be subscribed to.
     * @param expectedMessageCount  Expected number of queued messages.
     */
    static void expectRestoredSession(const std::string& clientId, const std::list<std::string>& topicNames, size_t expectedMessageCount);

    /**
     * @brief Restart the server against the same storage, without clearing it.
     * @returns Restarted server.
     */
    static SServer restartServer();
};

} // namespace xmq