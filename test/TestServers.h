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
        PostgreSQL,
        SQLite3
    };

    static std::shared_ptr<sptk::Host> getServerHost(uint16_t port, const std::string& hostname = "localhost");
    static sptk::String                dbConnectString(StorageType type);

    /**
     * @brief The Redis the tests actually connect to.
     * @remarks Named here rather than written into each suite's configuration so that the cleanup
     *          after every test and the servers under test cannot drift apart.
     *
     *          XMQ_TEST_REDIS overrides it. The default is one host shared by both desktops and by
     *          every build-farm container, and the cleanup after each test empties it - so two
     *          runs at once destroy each other rather than merely colliding. Pointing one of them
     *          at a Redis of its own is what makes it possible to debug a test while the farm is
     *          building.
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
