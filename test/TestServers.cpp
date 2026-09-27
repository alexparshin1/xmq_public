/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "TestServers.h"
#include <sptk5/net/RedisConnect.h>

#include <cstdlib>

#include "common/DirectoryNames.h"

using namespace std;
using namespace sptk;

namespace xmq {

shared_ptr<Host> TestServers::getServerHost(const uint16_t port, const std::string& hostname)
{
    // Function-local statics, not namespace/class statics: getServerHost is reached during static
    // initialization - a file-scope global in Session_ContinueAfterRestart.cpp calls
    // dbConnectString() - and the initialization order of statics across translation units is
    // unspecified. Constructing these on first use guarantees they exist before they are read,
    // instead of operating on an as-yet-unconstructed map and access-violating at process start.
    static const string                    defaultServer("127.0.0.1");
    static map<uint16_t, shared_ptr<Host>> hosts;

    if (const auto& iterator = hosts.find(port);
        iterator != hosts.end())
    {
        return iterator->second;
    }

    const auto host = localhostHasListener(port)
                          ? make_shared<Host>(hostname, port)
                          : make_shared<Host>(defaultServer, port);
    return host;
}

bool TestServers::localhostHasListener(const uint16_t port)
{
    try
    {
        TCPSocket socket;
        socket.open(Host("localhost", port));
        socket.close();
    }
    catch (const Exception&)
    {
        return false;
    }
    return true;
}

String TestServers::dbConnectString(const StorageType type)
{
    String connectString;
    switch (type)
    {
        case StorageType::PostgreSQL: {
            constexpr auto defaultPostgreSqlPort = 5432;
            const auto&    host = getServerHost(defaultPostgreSqlPort);
            connectString = "postgresql://gtest:test#123@" + host->hostname() + "/xmq_test";
            break;
        }
        case StorageType::SQLite3:
            connectString = DirectoryNames::sqliteUri(DirectoryNames::tempDirectory() / "xmq_test.db");
            break;
        case StorageType::Dummy:
            break;
    }
    return connectString;
} // namespace xmq

String TestServers::redisUri()
{
    if (const auto* fromEnvironment = ::getenv("XMQ_TEST_REDIS");
        fromEnvironment != nullptr && *fromEnvironment != 0)
    {
        return fromEnvironment;
    }
    return "redis://redis_server:6379";
}

void TestServers::flushRedis() noexcept
{
    try
    {
        RedisConnect redis;
        redis.connect(URL(redisUri()));
        redis.flush();
        redis.disconnect();
    }
    catch (const Exception&)
    {
        // Nothing to clean if there is nothing to connect to.
    }
}

} // namespace xmq
