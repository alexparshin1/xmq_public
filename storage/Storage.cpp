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

#include "Storage.h"
#include "RedisConnection.h"
#include "server/Server.h"

#include <sptk5/cutils>

using namespace std;
using namespace xmq;
using namespace sptk;

namespace {
// Long enough that the round trip costs nothing in aggregate, short enough that host clock drift
// between samples stays far inside any lease margin built on top of this.
constexpr auto ClockResampleInterval = std::chrono::seconds(30);
} // namespace

DateTime Storage::getCurrentTime() const
{
    return DateTime::Now() + storageClockOffset();
}

std::chrono::milliseconds Storage::storageClockOffset() const
{
    const scoped_lock lock(m_clockMutex);

    const auto localNow = DateTime::Now();
    if (!m_clockSampledAt.zero() && localNow - m_clockSampledAt < ClockResampleInterval)
    {
        return m_clockOffset;
    }

    const auto before = DateTime::Now();
    const auto storageTime = RedisConnection::readServerTime(m_redis);
    const auto after = DateTime::Now();

    if (storageTime.zero())
    {
        // Backend unreachable, or the command failed. Keep the offset already sampled rather than
        // snapping back to host time: a slightly stale offset is much closer to the truth than none,
        // and a clock that jumps whenever storage hiccups would be worse than one that lags.
        return m_clockOffset;
    }

    // The reply was produced at some point inside the round trip. Taking the midpoint cancels the
    // symmetric part of the latency instead of charging all of it to the offset, which would
    // otherwise make the storage clock look consistently later than it is.
    const auto midpoint = before + chrono::duration_cast<chrono::milliseconds>((after - before) / 2);

    m_clockOffset = chrono::duration_cast<chrono::milliseconds>(storageTime - midpoint);
    m_clockSampledAt = localNow;

    return m_clockOffset;
}

shared_ptr<Storage> Storage::create(Server* server, const string& redisConnectString, const bool reset)
{
    auto storage = make_shared<Storage>(server);
    storage->initialize(redisConnectString, reset);
    return storage;
}

void Storage::logError(const std::string& message) const
{
    m_server->logMessage(LogSubject::StorageEvents, LogPriority::Error, [&message]
                         {
                             return message;
                         });
}

Storage::~Storage()
{
    disconnect();
}

void Storage::connect(const string& redisConnectString)
{
    // initialize() drops the connection it replaces, and connects to the whole URL.
    initialize(redisConnectString, false);
}

void Storage::disconnect()
{
    if (m_redis)
    {
        m_redis->disconnect();
        m_redis = nullptr;
        m_persistent = false;
    }
}

void Storage::initialize(const string& redisConnectString, const bool reset)
{
    disconnect();

    if (!redisConnectString.empty())
    {
        // The whole URL, not its host and port: the credentials and the database it names are part
        // of it. Reduced to host and port, this connection went to database 0 while every other one
        // went where the configuration said, and it could not log in to a Redis with a password.
        m_redis = make_shared<RedisConnect>();
        m_redis->connect(URL(redisConnectString));
    }

    if (reset && m_redis)
    {
        RedisConnection::clearNodeState(m_redis, m_server->getNodeName());
    }

    m_persistent = m_redis != nullptr;
}

String Storage::toString() const
{
    return format("Redis on {}", m_redis->toString());
}
