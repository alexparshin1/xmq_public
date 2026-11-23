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

#include <sptk5/cdatabase>
#include <sptk5/cnet>

namespace xmq {

class TestServers
{
public:
    enum class StorageType
    {
        Dummy,
        SQLite3
    };

    static std::shared_ptr<sptk::Host> getServerHost(uint16_t port, const std::string& hostname = "localhost");
    static sptk::String                dbConnectString(StorageType type);

    /**
     * @brief The test database name on the shared SQL servers.
     * @remarks XMQ_TEST_DATABASE overrides it. The farm runs the Linux and FreeBSD sequences at
     *          the same time, and both drop and recreate the same tables; each sequence must use a
     *          different database or they destroy each other's schemas.
     */
    static std::string                 databaseName();

    /// A URI on the shared SQL test server, using databaseName().
    static sptk::String                sharedDatabaseUri(const std::string& host,
                                                        const std::string& scheme = "postgresql");

    /**
     * @brief The Redis the tests actually connect to.
     * @remarks Named here rather than written into each suite's configuration so that the cleanup
     *          after every test and the servers under test cannot drift apart.
     *
     *          XMQ_TEST_REDIS overrides it. The default is database 2 of the Redis shared by both
     *          desktops and every build-farm container; the farm sets database 1. The cleanup after
     *          each test is FLUSHDB, so runs in different databases leave each other alone - two
     *          runs in the same one still destroy each other, so a second local run at once wants
     *          a database of its own: redis://redis_server:6379/3.
     */
    static sptk::String redisUri();

    /**
     * @brief Empties that Redis with FLUSHDB.
     * @remarks Sessions, queued messages and node membership all outlive the server that wrote
     *          them, so whatever a test leaves behind is visible to the next one - and has made
     *          tests fail depending on what ran before them. Failures are swallowed: a machine
     *          with no Redis must still be able to run the suites that do not need one.
     */
    static void flushRedis() noexcept;

private:
    static bool localhostHasListener(uint16_t port);
};

} // namespace xmq
