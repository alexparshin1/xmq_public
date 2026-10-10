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
#include "NodeIdentity.h"

#include <sptk5/net/RedisCommand.h>

using namespace std;
using namespace sptk;
using namespace xmq::cluster;

namespace {

const string MembersKey = "cluster:members";
const string AdmissionKey = "cluster:admission";
const string NamesKey = "cluster:names";
const string NodePrefix = "cluster:node:";
const string AlivePrefix = "cluster:alive:";
const string TermKey = "cluster:term";
const string CoordinatorKey = "cluster:coordinator";

// What JoinScript answers.
constexpr int64_t Joined = 1;
constexpr int64_t ClusterFull = 0;
constexpr int64_t NameTaken = -1;

// Admit a node, or let a member back in, and take its lease. A member is let back in whatever the
// count, which is how a node that was down rejoins. Refused: a name another node has (-1), a
// cluster that is full (0), and a lease another run of this node holds (-2).
const string JoinScript = R"(
local owner = redis.call('HGET', KEYS[3], ARGV[2])
if owner and owner ~= ARGV[1] then return -1 end
local run = redis.call('GET', KEYS[5])
if run and run ~= ARGV[5] then return -2 end
if not redis.call('ZSCORE', KEYS[1], ARGV[1]) then
  if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[4]) then return 0 end
  redis.call('ZADD', KEYS[1], redis.call('INCR', KEYS[2]), ARGV[1])
end
local old = redis.call('HGET', KEYS[4], 'name')
if old and old ~= ARGV[2] and redis.call('HGET', KEYS[3], old) == ARGV[1] then redis.call('HDEL', KEYS[3], old) end
redis.call('HSET', KEYS[3], ARGV[2], ARGV[1])
redis.call('HSET', KEYS[4], 'name', ARGV[2], 'host_port', ARGV[3])
redis.call('SET', KEYS[5], ARGV[5], 'PX', ARGV[6])
return 1)";

// A member for a node that is not running: a GUID, a name and a place, but no lease.
const string AdmitAbsentScript = R"(
if redis.call('HEXISTS', KEYS[3], ARGV[2]) == 1 then return -1 end
if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[3]) then return 0 end
redis.call('ZADD', KEYS[1], redis.call('INCR', KEYS[2]), ARGV[1])
redis.call('HSET', KEYS[3], ARGV[2], ARGV[1])
redis.call('HSET', KEYS[4], 'name', ARGV[2], 'host_port', '')
return 1)";

// Move a member to the end of the succession order.
const string RequeueScript = R"(
if not redis.call('ZSCORE', KEYS[1], ARGV[1]) then return 0 end
redis.call('ZADD', KEYS[1], redis.call('INCR', KEYS[2]), ARGV[1])
return 1)";

// A step: renew this node's lease - while it is a member, and unless another run of it holds the
// lease - and read the coordinator, its name, and the places of this node and of the coordinator in
// the order, all at one moment.
const string StatusScript = R"(
local rank = redis.call('ZRANK', KEYS[2], ARGV[1]) or -1
local own = 1
if rank >= 0 then
  local run = redis.call('GET', KEYS[3])
  if run and run ~= ARGV[2] then own = 0 else redis.call('SET', KEYS[3], ARGV[2], 'PX', ARGV[3]) end
end
local coordinator = redis.call('GET', KEYS[1]) or ''
local id = string.match(coordinator, ' (.+)$')
local coordinatorRank = -1
local coordinatorName = ''
if id then
  coordinatorRank = redis.call('ZRANK', KEYS[2], id) or -1
  coordinatorName = redis.call('HGET', ARGV[4] .. id, 'name') or ''
end
return {coordinator, rank, coordinatorRank, coordinatorName, own})";

// Become coordinator if there is none, in a new term.
const string AcquireScript = R"(
if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end
local term = redis.call('INCR', KEYS[2])
redis.call('SET', KEYS[1], term .. ' ' .. ARGV[1], 'PX', ARGV[2])
return term)";

// Renew the coordinator key, if it is still ours.
const string RenewScript = R"(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('PEXPIRE', KEYS[1], ARGV[2])
return 1)";

// Leave: give the coordinator key up if it is ours, and free the place, the name and the lease.
const string LeaveScript = R"(
if redis.call('GET', KEYS[1]) == ARGV[3] then redis.call('DEL', KEYS[1]) end
redis.call('ZREM', KEYS[2], ARGV[1])
if redis.call('HGET', KEYS[3], ARGV[2]) == ARGV[1] then redis.call('HDEL', KEYS[3], ARGV[2]) end
redis.call('DEL', KEYS[4], KEYS[5])
return 1)";

// Take a session's ownership: free, ours already, or held by a node that is gone. Answers '' when it
// is ours now, else the holder's GUID.
const string ClaimSessionScript = R"(
local owner = redis.call('GET', KEYS[1])
if (not owner) or owner == ARGV[1] or redis.call('EXISTS', ARGV[2] .. owner) == 0 then
  redis.call('SET', KEYS[1], ARGV[1])
  return ''
end
return owner)";

// Give a session this node holds to the node that asked for it.
const string HandOverSessionScript = R"(
if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('SET', KEYS[1], ARGV[2]) end
return 1)";

// The members in order: name, TLS address and whether it holds its lease, three values each.
const string MembersScript = R"(
local out = {}
for _, id in ipairs(redis.call('ZRANGE', KEYS[1], 0, -1)) do
  local node = redis.call('HMGET', ARGV[1] .. id, 'name', 'host_port')
  table.insert(out, node[1] or '')
  table.insert(out, node[2] or '')
  table.insert(out, redis.call('EXISTS', ARGV[2] .. id))
end
return out)";

int64_t ticks(const chrono::steady_clock::time_point time)
{
    return time.time_since_epoch().count();
}

} // namespace

Coordinator::Coordinator(string nodeId, string nodeName, string hostPort, URL redisUrl,
                         const chrono::milliseconds lease, StateChanged stateChanged)
    : m_nodeId(std::move(nodeId))
    , m_nodeName(std::move(nodeName))
    , m_hostPort(std::move(hostPort))
    , m_runId(NodeIdentity::generate())
    , m_redisUrl(std::move(redisUrl))
    , m_lease(lease)
    // Five steps a lease: a step that is late, or a Redis command that is slow, still leaves four.
    , m_tick(lease / 5)
    , m_stateChanged(std::move(stateChanged))
{
}

Coordinator::~Coordinator()
{
    stop();
}

void Coordinator::connectRedis()
{
    if (m_redis.isConnected())
    {
        return;
    }

    // Within a step, not the system's own connect timeout: storage the network has lost sends no
    // refusal, and a connect left to the system waits it out - minutes, with this node's mutex held
    // and no step taken, so the node came back that much later than the storage did. A timeout the
    // URL names is kept when it is shorter.
    auto timeout = chrono::duration_cast<chrono::milliseconds>(m_tick);
    if (const auto& params = m_redisUrl.params(); params.has("connect_timeout"))
    {
        timeout = min(timeout, chrono::duration_cast<chrono::milliseconds>(chrono::seconds(params.get("connect_timeout").toInt())));
    }
    const auto& [host, port] = m_redisUrl.hostAndPort();
    m_redis.connect(host, port, m_redisUrl.username(), m_redisUrl.password(), m_redisUrl.path(), timeout);
}

vector<Variant> Coordinator::eval(const string& script, const vector<string>& keys, const vector<string>& arguments)
{
    if (m_storageLost.load())
    {
        throw Exception("Storage loss simulated by a test");
    }
    while (m_storageHung.load())
    {
        this_thread::sleep_for(10ms);
    }
    connectRedis();
    RedisCommand command("EVAL", script);
    command.emplace_back(to_string(keys.size()));
    command.emplace_back(keys);
    command.emplace_back(arguments);
    vector<Variant> results;
    m_redis.executeCommand(command, results);
    return results;
}

void Coordinator::start()
{
    {
        const scoped_lock lock(m_mutex);
        if (m_thread.joinable())
        {
            return;
        }

        // A node that crashed and was started again finds the lease of its previous run, which
        // serves nothing but holds the lease until it expires. Any longer, and another process has
        // this node's GUID.
        const auto giveUpAt = Clock::now() + m_lease + m_tick;
        for (;;)
        {
            const auto joined = eval(JoinScript,
                                     {MembersKey, AdmissionKey, NamesKey, NodePrefix + m_nodeId, AlivePrefix + m_nodeId},
                                     {m_nodeId, m_nodeName, m_hostPort, to_string(MaxMembers), m_runId,
                                      to_string(m_lease.count())});
            const auto answer = joined.empty() ? ClusterFull : joined[0].asInt64();
            if (answer == Joined)
            {
                break;
            }
            if (answer == NameTaken)
            {
                throw Exception(format("Another node of the cluster is called '{}'.", m_nodeName));
            }
            if (answer == ClusterFull)
            {
                throw Exception(format("The cluster has {} nodes already, the most it supports.", MaxMembers));
            }
            if (Clock::now() > giveUpAt)
            {
                throw Exception(format("Node '{}' ({}) is running elsewhere: another process holds its lease.",
                                       m_nodeName, m_nodeId));
            }
            this_thread::sleep_for(m_tick);
        }

        m_stopping = false;
        m_participating = true;
        step();

        // Installed before the lock is let go, so that the question above - is it running already? -
        // means something. Assigned after it, two callers arriving together - the node's own start
        // and an attach over a link - both passed the question, and the second assignment, to a
        // running std::thread, ended the process. The thread waits for this lock before its first
        // step, so starting it here costs nothing.
        m_thread = thread([this] { run(); });
        {
            const scoped_lock watchLock(m_watchMutex);
            m_stopWatch = false;
        }
        m_watch = thread([this] { watch(); });
    }
    reportState();
}

void Coordinator::stop()
{
    {
        const scoped_lock lock(m_watchMutex);
        m_stopWatch = true;
    }
    m_watchWake.notify_all();
    if (m_watch.joinable() && m_watch.get_id() != this_thread::get_id())
    {
        m_watch.join();
    }

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
        (void) eval(LeaveScript, {CoordinatorKey, MembersKey, NamesKey, NodePrefix + m_nodeId, AlivePrefix + m_nodeId},
                    {m_nodeId, m_nodeName, format("{} {}", m_term.load(), m_nodeId)});
    }
    catch (const Exception&)
    {
        // Redis is gone: the membership stays until someone removes it, which is what an
        // unreachable member's does anyway.
    }
    m_coordinator = false;
    m_leaseUntil = 0;
    // Out of the cluster, the node is on its own again and serves its clients as any broker does.
    m_participating = false;
    m_reportedOnline = true;
}

void Coordinator::simulateStorageLoss(const bool lost)
{
    m_storageLost = lost;
}

void Coordinator::simulateStorageHang(const bool hung)
{
    m_storageHung = hung;
}

bool Coordinator::isOnline() const
{
    if (!m_participating.load())
    {
        return true;
    }
    return ticks(Clock::now()) < m_leaseUntil.load();
}

string Coordinator::coordinatorName() const
{
    const scoped_lock lock(m_mutex);
    return m_coordinatorName;
}

vector<Coordinator::Member> Coordinator::members()
{
    const scoped_lock lock(m_mutex);
    const auto        results = eval(MembersScript, {MembersKey}, {NodePrefix, AlivePrefix});
    vector<Member>    members;
    for (size_t i = 0; i + 2 < results.size(); i += 3)
    {
        members.push_back({results[i].asString().c_str(), results[i + 1].asString().c_str(), results[i + 2].asInt64() == 1});
    }
    return members;
}

bool Coordinator::isMember(const string& nodeName)
{
    const scoped_lock lock(m_mutex);
    connectRedis();
    const auto id = m_redis.getHashValue(NamesKey, nodeName);
    if (id.isNull() || id.asString().empty())
    {
        return false;
    }
    RedisCommand command("ZSCORE", MembersKey);
    command.emplace_back(string(id.asString().c_str()));
    vector<Variant> results;
    m_redis.executeCommand(command, results);
    return !results.empty() && !results[0].isNull();
}

bool Coordinator::admitAbsentNode(const string& nodeName)
{
    const scoped_lock lock(m_mutex);
    const auto        id = NodeIdentity::generate();
    const auto        admitted = eval(AdmitAbsentScript, {MembersKey, AdmissionKey, NamesKey, NodePrefix + id},
                                      {id, nodeName, to_string(MaxMembers)});
    return !admitted.empty() && admitted[0].asInt64() == Joined;
}

string Coordinator::claimSession(const string& clientId)
{
    const scoped_lock lock(m_mutex);
    const auto        result = eval(ClaimSessionScript, {format("session_{}_owner", clientId)}, {m_nodeId, AlivePrefix});
    return result.empty() ? string() : string(result[0].asString().c_str());
}

void Coordinator::handOverSession(const string& clientId, const string& nodeId)
{
    const scoped_lock lock(m_mutex);
    (void) eval(HandOverSessionScript, {format("session_{}_owner", clientId)}, {m_nodeId, nodeId});
}

string Coordinator::nodeName(const string& nodeId)
{
    const scoped_lock lock(m_mutex);
    connectRedis();
    const auto name = m_redis.getHashValue(NodePrefix + nodeId, "name");
    return name.isNull() ? string() : string(name.asString().c_str());
}

int Coordinator::membership(RedisConnect& redis, const string& nodeId)
{
    vector<Variant> results;
    redis.executeCommand(RedisCommand("ZCARD", MembersKey), results);
    if (results.empty() || results[0].asInt64() == 0)
    {
        return -1;
    }
    RedisCommand command("ZSCORE", MembersKey);
    command.emplace_back(nodeId);
    results.clear();
    redis.executeCommand(command, results);
    return !results.empty() && !results[0].isNull() ? 1 : 0;
}

void Coordinator::clearClusterState(RedisConnect& redis)
{
    if (const auto keys = redis.scan("cluster:*", 1000); !keys.empty())
    {
        (void) redis.deleteKeys(keys);
    }
}

void Coordinator::run()
{
    unique_lock lock(m_mutex);
    while (!m_stopping)
    {
        m_wake.wait_for(lock, m_tick, [this] { return m_stopping; });
        if (m_stopping)
        {
            break;
        }
        step();
    }
}

void Coordinator::watch()
{
    // Apart from the steps, which wait on Redis - up to its read timeout, longer than a lease - with
    // the step's lock held. The lease is a deadline, and a node whose storage has gone has to stop
    // serving when it passes, not when the step that is waiting on that storage gives up: reported
    // from the step, the clients of a node that had lost its storage stayed connected for as long
    // as Redis kept the step waiting. Only atomics are read here, so nothing a step holds can delay it.
    const auto interval = min(chrono::duration_cast<chrono::milliseconds>(m_tick) / 4, chrono::milliseconds(250));
    unique_lock lock(m_watchMutex);
    while (!m_stopWatch)
    {
        m_watchWake.wait_for(lock, interval, [this] { return m_stopWatch; });
        lock.unlock();
        reportState();
        lock.lock();
    }
}

void Coordinator::step()
{
    // Taken before Redis is asked: the lease is counted from a moment no later than Redis's own
    // start of it, so this node stops serving no later than Redis lets it expire - and no later than
    // another node may take over what it serves.
    const auto started = Clock::now();
    try
    {
        const auto status = eval(StatusScript, {CoordinatorKey, MembersKey, AlivePrefix + m_nodeId},
                                 {m_nodeId, m_runId, to_string(m_lease.count()), NodePrefix});
        if (status.size() < 5)
        {
            throw Exception("Unexpected cluster status from Redis.");
        }
        auto       coordinator = string(status[0].asString().c_str());
        const auto rank = status[1].asInt64();
        const auto coordinatorRank = status[2].asInt64();
        auto       coordinatorName = string(status[3].asString().c_str());
        const auto ownLease = status[4].asInt64() == 1;

        if (rank < 0 || !ownLease)
        {
            // Not a member - removed while it was running, or the storage was wiped - or another
            // process runs this node and holds its lease. Either way it serves nothing in the
            // cluster's name.
            m_coordinatorName.clear();
            m_coordinator = false;
            m_leaseUntil = 0;
            return;
        }
        m_leaseUntil = ticks(started + m_lease);

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
                if (const auto acquired = eval(AcquireScript, {CoordinatorKey, TermKey}, {m_nodeId, to_string(m_lease.count())});
                    !acquired.empty() && acquired[0].asInt64() > 0)
                {
                    coordinator = format("{} {}", acquired[0].asInt64(), m_nodeId);
                    coordinatorName = m_nodeName;
                }
            }
        }
        else
        {
            m_coordinatorAbsentSince = {};
        }

        int64_t term = 0;
        string  coordinatorId;
        if (const auto space = coordinator.find(' '); space != string::npos)
        {
            term = stoll(coordinator.substr(0, space));
            coordinatorId = coordinator.substr(space + 1);
        }

        if (coordinatorId == m_nodeId)
        {
            const auto renewed = eval(RenewScript, {CoordinatorKey}, {coordinator, to_string(m_lease.count())});
            m_coordinator = !renewed.empty() && renewed[0].asInt64() == 1;
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
                (void) eval(RequeueScript, {MembersKey, AdmissionKey}, {m_nodeId});
            }
        }

        m_coordinatorName = coordinatorName;
        m_term = term;
    }
    catch (const Exception&)
    {
        // The connection is dropped, to be opened again next step. The node goes on serving until
        // its lease would have expired: until then nobody may take over its sessions, so a Redis
        // outage shorter than a lease costs its clients nothing.
        m_redis.disconnect();
        m_coordinator = false;
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
