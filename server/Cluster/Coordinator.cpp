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

#include "Coordinator.h"

#include <sptk5/net/RedisCommand.h>

using namespace std;
using namespace sptk;
using namespace xmq::cluster;

namespace {

const string MembersKey = "cluster:members";
const string AdmissionKey = "cluster:admission";
const string TermKey = "cluster:term";
const string CoordinatorKey = "cluster:coordinator";
const string LeasePrefix = "cluster:lease:";

// Admit a node unless the cluster is full. A member is let back in whatever the count, which is
// how a node that was down rejoins.
const string AdmitScript = R"(
if redis.call('ZSCORE', KEYS[1], ARGV[1]) then return 1 end
if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[2]) then return 0 end
redis.call('ZADD', KEYS[1], redis.call('INCR', KEYS[2]), ARGV[1])
return 1)";

// Move a member to the end of the succession order: a former coordinator rejoins behind everyone.
const string RequeueScript = R"(
if not redis.call('ZSCORE', KEYS[1], ARGV[1]) then return 0 end
redis.call('ZADD', KEYS[1], redis.call('INCR', KEYS[2]), ARGV[1])
return 1)";

// Everything a step reads, at one moment: the coordinator, this node's place in the order and the
// coordinator's, and this node's lease.
const string StatusScript = R"(
local coordinator = redis.call('GET', KEYS[1]) or ''
local rank = redis.call('ZRANK', KEYS[2], ARGV[1]) or -1
local coordinatorRank = -1
local name = string.match(coordinator, ' (.+)$')
if name then coordinatorRank = redis.call('ZRANK', KEYS[2], name) or -1 end
local lease = redis.call('GET', KEYS[3]) or ''
return {coordinator, rank, coordinatorRank, lease, redis.call('PTTL', KEYS[3])})";

// Become coordinator if there is none, in a new term.
const string AcquireScript = R"(
if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end
local term = redis.call('INCR', KEYS[2])
redis.call('SET', KEYS[1], term .. ' ' .. ARGV[1], 'PX', ARGV[2])
return term)";

// Renew the coordinator key, if it is still ours, and in the same step the leases of the nodes
// named, with the same TTL: a node lease never outlives the authority that gave it.
const string RenewScript = R"(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('PEXPIRE', KEYS[1], ARGV[2])
for i = 5, #ARGV do redis.call('SET', ARGV[4] .. ARGV[i], ARGV[3], 'PX', ARGV[2]) end
return 1)";

// Give the coordinator key up, if it is ours.
const string ReleaseScript = R"(
if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('DEL', KEYS[1]) end
return 1)";

int64_t ticks(const chrono::steady_clock::time_point time)
{
    return time.time_since_epoch().count();
}

} // namespace

Coordinator::Coordinator(string nodeName, URL redisUrl, const chrono::milliseconds lease,
                         ConnectedPeers connectedPeers, StateChanged stateChanged)
    : m_nodeName(std::move(nodeName))
    , m_redisUrl(std::move(redisUrl))
    , m_lease(lease)
    // Five steps a lease: a step that is late, or a Redis command that is slow, still leaves four.
    , m_tick(lease / 5)
    , m_connectedPeers(std::move(connectedPeers))
    , m_stateChanged(std::move(stateChanged))
{
}

Coordinator::~Coordinator()
{
    stop();
}

void Coordinator::connectRedis()
{
    if (!m_redis.isConnected())
    {
        m_redis.connect(m_redisUrl);
    }
}

vector<Variant> Coordinator::eval(const string& script, const vector<string>& keys, const vector<string>& arguments)
{
    connectRedis();
    RedisCommand command("EVAL", script);
    command.emplace_back(to_string(keys.size()));
    command.emplace_back(keys);
    command.emplace_back(arguments);
    vector<Variant> results;
    m_redis.executeCommand(command, results);
    return results;
}

bool Coordinator::admit(const string& nodeName)
{
    const scoped_lock lock(m_mutex);
    const auto        admitted = eval(AdmitScript, {MembersKey, AdmissionKey}, {nodeName, to_string(MaxMembers)});
    return !admitted.empty() && admitted[0].asInt64() == 1;
}

void Coordinator::start()
{
    {
        const scoped_lock lock(m_mutex);
        if (m_thread.joinable())
        {
            return;
        }
        if (const auto admitted = eval(AdmitScript, {MembersKey, AdmissionKey}, {m_nodeName, to_string(MaxMembers)});
            admitted.empty() || admitted[0].asInt64() != 1)
        {
            throw Exception(format("The cluster has {} nodes already, the most it supports.", MaxMembers));
        }
        m_graceUntil = ticks(Clock::now() + m_lease);
        m_stopping = false;
        m_participating = true;
        step();
    }
    reportState();
    m_thread = thread([this] { run(); });
}

void Coordinator::stop()
{
    {
        const scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable() && m_thread.get_id() != this_thread::get_id())
    {
        m_thread.join();
    }
}

void Coordinator::leave()
{
    stop();
    const scoped_lock lock(m_mutex);
    try
    {
        if (m_coordinator)
        {
            (void) eval(ReleaseScript, {CoordinatorKey}, {format("{} {}", m_term.load(), m_nodeName)});
        }
        connectRedis();
        vector<Variant> results;
        RedisCommand    removeMember("ZREM", MembersKey);
        removeMember.emplace_back(m_nodeName);
        m_redis.executeCommand(removeMember, results);
        (void) m_redis.deleteKeys({LeasePrefix + m_nodeName});
    }
    catch (const Exception&)
    {
        // Redis is gone: the membership stays until someone removes it, which is what an
        // unreachable member's does anyway.
    }
    m_coordinator = false;
    m_leaseUntil = 0;
    m_graceUntil = 0;
    // Out of the cluster, the node is on its own again and serves its clients as any broker does.
    m_participating = false;
    m_reportedOnline = true;
}

void Coordinator::nudge()
{
    {
        const scoped_lock lock(m_mutex);
        m_nudged = true;
    }
    m_wake.notify_all();
}

bool Coordinator::isOnline() const
{
    if (!m_participating.load())
    {
        return true;
    }
    const auto now = ticks(Clock::now());
    return now < m_leaseUntil.load() || now < m_graceUntil.load();
}

string Coordinator::coordinatorName() const
{
    const scoped_lock lock(m_mutex);
    return m_coordinatorName;
}

vector<string> Coordinator::members()
{
    const scoped_lock lock(m_mutex);
    connectRedis();
    RedisCommand command("ZRANGE", MembersKey);
    command.emplace_back("0");
    command.emplace_back("-1");
    vector<Variant> results;
    m_redis.executeCommand(command, results);
    vector<string> names;
    for (const auto& result: results)
    {
        names.emplace_back(result.asString().c_str());
    }
    return names;
}

void Coordinator::clearClusterState(RedisConnect& redis)
{
    auto keys = redis.scan("cluster:*", 1000);
    if (!keys.empty())
    {
        (void) redis.deleteKeys(keys);
    }
}

void Coordinator::run()
{
    unique_lock lock(m_mutex);
    while (!m_stopping)
    {
        m_wake.wait_for(lock, m_tick, [this] { return m_stopping || m_nudged; });
        if (m_stopping)
        {
            break;
        }
        m_nudged = false;
        step();
        lock.unlock();
        reportState();
        lock.lock();
    }
}

void Coordinator::step()
{
    // Taken before Redis is asked: the lease is counted from a moment no later than Redis's own
    // reading of it, so this node gives up its lease no later than Redis expires it.
    const auto started = Clock::now();
    try
    {
        const auto status = eval(StatusScript, {CoordinatorKey, MembersKey, LeasePrefix + m_nodeName}, {m_nodeName});
        if (status.size() < 5)
        {
            throw Exception("Unexpected cluster status from Redis.");
        }
        auto       coordinator = string(status[0].asString().c_str());
        const auto rank = status[1].asInt64();
        const auto coordinatorRank = status[2].asInt64();
        const auto lease = string(status[3].asString().c_str());
        const auto leaseLeft = chrono::milliseconds(status[4].asInt64());

        if (rank < 0)
        {
            // Not a member: its record was removed while it was running, or the storage was
            // wiped. It takes no part until it is admitted again.
            m_coordinatorName.clear();
            m_coordinator = false;
            m_leaseUntil = 0;
            return;
        }

        if (coordinator.empty())
        {
            if (m_coordinatorAbsentSince == Clock::time_point {})
            {
                m_coordinatorAbsentSince = started;
            }
            // The most senior node takes it at once, each one after it a tick later, so the first
            // in the order that is alive wins without the others having to agree on who that is.
            if (started - m_coordinatorAbsentSince >= m_tick * rank)
            {
                if (const auto acquired = eval(AcquireScript, {CoordinatorKey, TermKey}, {m_nodeName, to_string(m_lease.count())});
                    !acquired.empty() && acquired[0].asInt64() > 0)
                {
                    coordinator = format("{} {}", acquired[0].asInt64(), m_nodeName);
                }
            }
        }
        else
        {
            m_coordinatorAbsentSince = {};
        }

        int64_t term = 0;
        string  coordinatorName;
        if (const auto space = coordinator.find(' '); space != string::npos)
        {
            term = stoll(coordinator.substr(0, space));
            coordinatorName = coordinator.substr(space + 1);
        }

        if (coordinatorName == m_nodeName)
        {
            vector<string> arguments {coordinator, to_string(m_lease.count()), to_string(term), LeasePrefix, m_nodeName};
            for (const auto& peer: m_connectedPeers())
            {
                arguments.push_back(peer);
            }
            const auto renewed = eval(RenewScript, {CoordinatorKey}, arguments);
            m_coordinator = !renewed.empty() && renewed[0].asInt64() == 1;
            if (m_coordinator)
            {
                m_leaseUntil = ticks(started + m_lease);
            }
        }
        else
        {
            m_coordinator = false;
            if (coordinatorRank >= 0 && rank < coordinatorRank)
            {
                // Ahead of the coordinator in the order: this node was coordinator, or was due to
                // be, and was passed over while it was down or cut off. It cannot claim that place
                // back, and goes to the end, behind every node admitted meanwhile. That keeps the
                // coordinator first in the order, so the next one is always the node after it.
                (void) eval(RequeueScript, {MembersKey, AdmissionKey}, {m_nodeName});
            }
            // A lease from an older term is no lease: its coordinator has been replaced.
            const auto valid = term > 0 && lease == to_string(term) && leaseLeft.count() > 0;
            m_leaseUntil = valid ? ticks(started + leaseLeft) : 0;
        }

        if (m_leaseUntil.load() != 0)
        {
            m_graceUntil = 0;
        }
        m_coordinatorName = coordinatorName;
        m_term = term;
    }
    catch (const Exception&)
    {
        // Without the shared storage the node may not serve clients, lease or no lease: it could
        // not keep the sessions it serves. The connection is dropped, to be opened again next step.
        m_redis.disconnect();
        m_coordinator = false;
        m_leaseUntil = 0;
        m_graceUntil = 0;
    }
}

void Coordinator::reportState()
{
    const auto online = isOnline();
    if (m_reportedOnline.exchange(online) != online && m_stateChanged)
    {
        m_stateChanged(online);
    }
}
