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
const string AdoptPrefix = "cluster:adopt:";

// The coordinator's: the sessions of members that are gone - their lease has expired - go to the
// members that are running, round the circle, each into the running member's adopt queue. The gone
// member's set is emptied in the same step, so nothing is handed out twice.
const string DistributeScript = R"(
local live = {}
local gone = {}
for _, id in ipairs(redis.call('ZRANGE', KEYS[1], 0, -1)) do
  if redis.call('EXISTS', ARGV[2] .. id) == 1 then table.insert(live, id) else table.insert(gone, id) end
end
if #live == 0 then return 0 end
local handed = 0
local removed = redis.call('HGETALL', 'cluster:recovery')
for i = 1, #removed, 2 do
  local id, name = removed[i], removed[i + 1]
  for _, client in ipairs(redis.call('SMEMBERS', 'node_' .. name .. '_sessions')) do
    redis.call('RPUSH', ARGV[3] .. live[(handed % #live) + 1], client)
    handed = handed + 1
  end
  for _, client in ipairs(redis.call('LRANGE', ARGV[3] .. id, 0, -1)) do
    redis.call('RPUSH', ARGV[3] .. live[(handed % #live) + 1], client)
    handed = handed + 1
  end
  redis.call('DEL', 'node_' .. name .. '_sessions', ARGV[3] .. id)
  redis.call('HDEL', 'cluster:recovery', id)
end
for _, id in ipairs(gone) do
  local name = redis.call('HGET', ARGV[1] .. id, 'name')
  if name then
    local key = 'node_' .. name .. '_sessions'
    for _, client in ipairs(redis.call('SMEMBERS', key)) do
      redis.call('RPUSH', ARGV[3] .. live[(handed % #live) + 1], client)
      handed = handed + 1
    end
    redis.call('DEL', key)
  end
end
return handed)";

// Take up to ARGV[1] sessions this node was given to adopt.
const string TakeAdoptedScript = R"(
local taken = redis.call('LRANGE', KEYS[1], 0, tonumber(ARGV[1]) - 1)
redis.call('LTRIM', KEYS[1], #taken, -1)
return taken)";

// Claim a session only if nobody running holds it: what adoption may take. Answers 1 when it is ours.
const string ClaimOrphanScript = R"(
local owner = redis.call('GET', KEYS[1])
if (not owner) or owner == ARGV[1] or redis.call('EXISTS', ARGV[2] .. owner) == 0 then
  redis.call('SET', KEYS[1], ARGV[1])
  return 1
end
return 0)";

// What JoinScript answers.
constexpr int64_t Joined = 1;
constexpr int64_t ClusterFull = 0;
constexpr int64_t NameTaken = -1;

// Admit a node, or let a member back in, and take its lease. A member is let back in whatever the
// count, which is how a node that was down rejoins. Refused: a name another node has (-1), a
// cluster that is full (0), and a lease another run of this node holds (-2).
const string JoinScript = R"(
if redis.call('SISMEMBER', 'cluster:revoked', ARGV[1]) == 1 then return -3 end
local recovery = redis.call('HGETALL', 'cluster:recovery')
for i = 1, #recovery, 2 do
  if recovery[i + 1] == ARGV[2] then return -1 end
end
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

const string RemoveOfflineScript = R"(
if redis.call('EXISTS', ARGV[2] .. ARGV[1]) == 1 then return 0 end
local name = redis.call('HGET', ARGV[3] .. ARGV[1], 'name')
redis.call('SADD', 'cluster:revoked', ARGV[1])
if name then
  redis.call('HSET', 'cluster:recovery', ARGV[1], name)
  if redis.call('HGET', KEYS[2], name) == ARGV[1] then redis.call('HDEL', KEYS[2], name) end
end
redis.call('ZREM', KEYS[1], ARGV[1])
local coordinator = redis.call('GET', KEYS[3])
if coordinator and string.match(coordinator, ' (.+)$') == ARGV[1] then redis.call('DEL', KEYS[3]) end
redis.call('DEL', ARGV[3] .. ARGV[1])
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

// Drop this node's ownership marker when the session that carried it is gone: a clean session
// ends at DISCONNECT, and without this the key the claim script wrote stays in Redis forever -
// one key per connect. Only this node's own marker is removed, so a session already handed over
// to another node is never undone by a late close.
const string ReleaseOwnershipScript = R"(
if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('DEL', KEYS[1]) end
return 1)";

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
  table.insert(out, id)
end
return out)";

const string SnapshotScript = "local function members()\n" + MembersScript + R"(
end
local out = members()
local coordinator = redis.call('GET', 'cluster:coordinator') or ''
local id = string.match(coordinator, ' (.+)$')
local name = ''
if id then name = redis.call('HGET', ARGV[1] .. id, 'name') or '' end
table.insert(out, 1, name)
return out)";

int64_t ticks(const chrono::steady_clock::time_point time)
{
    return time.time_since_epoch().count();
}

} // namespace

Coordinator::Coordinator(string nodeId, string nodeName, string hostPort, URL redisUrl,
                         const chrono::milliseconds lease, StateChanged stateChanged, ErrorHandler errorHandler)
    : m_nodeId(std::move(nodeId))
    , m_nodeName(std::move(nodeName))
    , m_hostPort(std::move(hostPort))
    , m_runId(NodeIdentity::generate())
    , m_redisUrl(std::move(redisUrl))
    , m_lease(lease)
    // Five steps a lease: a step that is late, or a Redis command that is slow, still leaves four.
    , m_tick(lease / 5)
    , m_stateChanged(std::move(stateChanged))
    , m_errorHandler(std::move(errorHandler))
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
    // And an answer within a step too: a connection the network lost answers nothing, and a step
    // waiting on it held this node's mutex until TCP gave up - the node came back that much later
    // than the storage did.
    m_redis.setReadTimeout(chrono::duration_cast<chrono::milliseconds>(m_tick));
    const auto& [host, port] = m_redisUrl.hostAndPort();
    m_redis.connect(host, port, m_redisUrl.username(), m_redisUrl.password(), m_redisUrl.path(), timeout);
}

vector<Variant> Coordinator::executeRedis(const RedisCommand& command)
{
    // A timed-out read may have consumed only part of a reply. Reusing that stream
    // pairs the next command with old data, even after Redis is reachable again.
    try
    {
        connectRedis();
        vector<Variant> results;
        m_redis.executeCommand(command, results);
        return results;
    }
    catch (...)
    {
        m_redis.disconnect();
        throw;
    }
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
    RedisCommand command("EVAL", script);
    command.emplace_back(to_string(keys.size()));
    command.emplace_back(keys);
    command.emplace_back(arguments);
    return executeRedis(command);
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
            if (answer == -3)
                throw Exception("This node was force removed from the cluster. Its old identity cannot rejoin.");
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
    // A failed removal is not a successful leave: otherwise restart finds the old membership
    // and silently rejoins. Let the caller report that storage could not confirm the operation.
    (void) eval(LeaveScript, {CoordinatorKey, MembersKey, NamesKey, NodePrefix + m_nodeId, AlivePrefix + m_nodeId},
                {m_nodeId, m_nodeName, format("{} {}", m_term.load(), m_nodeId)});
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
    for (size_t i = 0; i + 3 < results.size(); i += 4)
    {
        members.push_back({results[i].asString().c_str(), results[i + 1].asString().c_str(),
                           results[i + 2].asInt64() == 1, results[i + 3].asString().c_str()});
    }
    return members;
}

bool Coordinator::removeOfflineMember(RedisConnect& redis, const string& nodeId)
{
    RedisCommand command("EVAL", RemoveOfflineScript);
    for (const auto& value: vector<string>{"3", MembersKey, NamesKey, CoordinatorKey,
                                          nodeId, AlivePrefix, NodePrefix}) command.emplace_back(value);
    vector<Variant> results;
    redis.executeCommand(command, results);
    if (results.size() != 1) throw Exception("Unexpected force removal response from Redis.");
    return results[0].asInt64() == 1;
}

Coordinator::Snapshot Coordinator::readSnapshot(RedisConnect& redis)
{
    vector<Variant> results;
    RedisCommand command("EVAL", SnapshotScript);
    command.emplace_back("1");
    command.emplace_back(MembersKey);
    command.emplace_back(NodePrefix);
    command.emplace_back(AlivePrefix);
    redis.executeCommand(command, results);
    if (results.empty() || (results.size() - 1) % 4 != 0)
    {
        throw Exception("Unexpected cluster snapshot from Redis.");
    }
    Snapshot snapshot;
    snapshot.m_coordinatorName = results[0].asString().c_str();
    for (size_t i = 1; i + 3 < results.size(); i += 4)
    {
        snapshot.m_members.push_back({results[i].asString().c_str(), results[i + 1].asString().c_str(),
                                      results[i + 2].asInt64() == 1, results[i + 3].asString().c_str()});
    }
    return snapshot;
}

bool Coordinator::isMember(const string& nodeName)
{
    const scoped_lock lock(m_mutex);
    RedisCommand lookup("HGET", NamesKey);
    lookup.emplace_back(nodeName);
    const auto found = executeRedis(lookup);
    const auto id = found.empty() ? Variant() : found.front();
    if (id.isNull() || id.asString().empty())
    {
        return false;
    }
    RedisCommand command("ZSCORE", MembersKey);
    command.emplace_back(string(id.asString().c_str()));
    const auto results = executeRedis(command);
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

void Coordinator::releaseSessionOwnership(const string& clientId)
{
    const scoped_lock lock(m_mutex);
    (void) eval(ReleaseOwnershipScript, {format("session_{}_owner", clientId)}, {m_nodeId});
}

bool Coordinator::claimOrphanSession(const string& clientId)
{
    const scoped_lock lock(m_mutex);
    const auto        result = eval(ClaimOrphanScript, {format("session_{}_owner", clientId)}, {m_nodeId, AlivePrefix});
    return !result.empty() && result[0].asInt64() == 1;
}

string Coordinator::sessionOwner(const string& clientId)
{
    const scoped_lock lock(m_mutex);
    const auto results = executeRedis(RedisCommand("GET", format("session_{}_owner", clientId)));
    const auto owner = results.empty() ? Variant() : results.front();
    return owner.isNull() ? string() : string(owner.asString().c_str());
}

void Coordinator::handOverSession(const string& clientId, const string& nodeId)
{
    const scoped_lock lock(m_mutex);
    (void) eval(HandOverSessionScript, {format("session_{}_owner", clientId)}, {m_nodeId, nodeId});
}

string Coordinator::nodeName(const string& nodeId)
{
    const scoped_lock lock(m_mutex);
    RedisCommand lookup("HGET", NodePrefix + nodeId);
    lookup.emplace_back("name");
    const auto results = executeRedis(lookup);
    const auto name = results.empty() ? Variant() : results.front();
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
        const auto readStatus = [this]
        {
            const auto status = eval(StatusScript, {CoordinatorKey, MembersKey, AlivePrefix + m_nodeId},
                                     {m_nodeId, m_runId, to_string(m_lease.count()), NodePrefix});
            if (status.size() != 5)
            {
                throw Exception("Unexpected cluster status from Redis.");
            }
            return make_tuple(string(status[0].asString().c_str()), status[1].asInt64(), status[2].asInt64(),
                              string(status[3].asString().c_str()), status[4].asInt64() == 1);
        };

        auto [coordinator, rank, coordinatorRank, coordinatorName, ownLease] = readStatus();

        if (rank < 0 || !ownLease)
        {
            // Not a member - removed while it was running, or the storage was wiped - or another
            // process runs this node and holds its lease. Either way it serves nothing in the
            // cluster's name.
            if (rank < 0)
            {
                // A node whose place went with the storage takes it back by itself. The
                // alternative is a node that was cut off once and then serves nothing until
                // somebody restarts it, which is the failure this cluster exists to avoid.
                // JoinScript answers "running elsewhere" when another process holds this node's
                // lease, so a clone still cannot come up beside its original: only a node whose
                // own lease is absent, or its own, is let back in.
                const auto joined = eval(JoinScript,
                                         {MembersKey, AdmissionKey, NamesKey, NodePrefix + m_nodeId, AlivePrefix + m_nodeId},
                                         {m_nodeId, m_nodeName, m_hostPort, to_string(MaxMembers), m_runId,
                                          to_string(m_lease.count())});
                if (!joined.empty() && joined[0].asInt64() == Joined)
                {
                    tie(coordinator, rank, coordinatorRank, coordinatorName, ownLease) = readStatus();
                }
            }

            if (rank < 0 || !ownLease)
            {
                m_coordinatorName.clear();
                m_coordinator = false;
                m_leaseUntil = 0;
                return;
            }
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
            const auto holds = !renewed.empty() && renewed[0].asInt64() == 1;
            // The term first: whoever sees this node coordinator sees the term it is coordinator in.
            if (holds)
            {
                m_term = term;
            }
            m_coordinator = holds;
            if (holds)
            {
                (void) eval(DistributeScript, {MembersKey}, {NodePrefix, AlivePrefix, AdoptPrefix});
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
                (void) eval(RequeueScript, {MembersKey, AdmissionKey}, {m_nodeId});
            }
        }

        // What the coordinator gave this node to adopt, a batch a step: the work itself is the
        // adopter's, on a thread of its own, and a step must stay short.
        if (m_adoptSessions)
        {
            constexpr int batch = 1000;
            const auto    taken = eval(TakeAdoptedScript, {AdoptPrefix + m_nodeId}, {to_string(batch)});
            vector<string> clients;
            for (const auto& client: taken)
            {
                if (!client.isNull())
                {
                    clients.emplace_back(client.asString().c_str());
                }
            }
            if (!clients.empty())
            {
                m_adoptSessions(std::move(clients));
            }
        }

        m_coordinatorName = coordinatorName;
        m_term = term;
    }
    catch (const std::exception& error)
    {
        // Also discard replies with an unexpected shape: they may be leftovers of
        // a command interrupted before this step. The next step opens a fresh stream.
        m_redis.disconnect();
        m_coordinator = false;
        if (m_errorHandler)
        {
            m_errorHandler(error.what());
        }
        // Keep the last confirmed deadline. The independent watcher stops service
        // when it expires, even while a Redis read is still waiting.
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
