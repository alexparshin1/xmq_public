/* SPDX-License-Identifier: MPL-2.0 */
#include "ReliableMessageStore.h"
#include "common/RedisCredentials.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
constexpr auto Timeout = chrono::seconds(5);

// Scripts do not roll back on error. Validate every key type before making any changes.
// The command set is deliberately closed: no unvalidated command can be submitted here.
constexpr auto Commit = R"(
local expectedTypes = {}
for i = 1, #KEYS do
    local base = (i - 1) * 3
    local kind = ARGV[base + 1]
    local expected = kind == 'SET' and 'string' or (kind == 'SADD' and 'set' or 'hash')
    if kind ~= 'SET' and kind ~= 'HSET' and kind ~= 'HDEL' and kind ~= 'SADD' then
        return redis.error_reply('unsupported publication operation')
    end
    if expectedTypes[KEYS[i]] and expectedTypes[KEYS[i]] ~= expected then
        return redis.error_reply('conflicting publication key types')
    end
    expectedTypes[KEYS[i]] = expected
    local allowed
    if kind == 'SET' then
        allowed = redis.acl_check_cmd(kind, KEYS[i], ARGV[base + 3])
    elseif kind == 'SADD' or kind == 'HDEL' then
        allowed = redis.acl_check_cmd(kind, KEYS[i], ARGV[base + 2])
    else
        allowed = redis.acl_check_cmd(kind, KEYS[i], ARGV[base + 2], ARGV[base + 3])
    end
    if not allowed then
        return redis.error_reply('publication operation denied by Redis ACL')
    end
    local actual = redis.call('TYPE', KEYS[i]).ok
    if actual ~= 'none' and actual ~= expected then
        return redis.error_reply('wrong publication key type')
    end
end
for i = 1, #KEYS do
    local base = (i - 1) * 3
    local kind = ARGV[base + 1]
    if kind == 'SET' then
        redis.call('SET', KEYS[i], ARGV[base + 3])
    elseif kind == 'SADD' then
        redis.call('SADD', KEYS[i], ARGV[base + 2])
    elseif kind == 'HSET' then
        redis.call('HSET', KEYS[i], ARGV[base + 2], ARGV[base + 3])
    else
        redis.call('HDEL', KEYS[i], ARGV[base + 2])
    end
end
return 1
)";
}

thread_local ReliableMessageStore::Transaction* ReliableMessageStore::Transaction::s_current = nullptr;

ReliableMessageStore::ReliableMessageStore(const URL& url)
{
    m_redis.setReadTimeout(Timeout + chrono::seconds(1));
    m_redis.connect(redisConnectionUrl(url));
    // WAITAOF fails when AOF is disabled or the command is unavailable. Fail before listening.
    sync(m_redis);
}

void ReliableMessageStore::sync(RedisConnect& redis)
{
    vector<Variant> result;
    RedisCommand command("WAITAOF", "1");
    command.emplace_back("0");
    command.emplace_back("5000");
    redis.executeCommand(command, result);
    if (result.size() != 2 || result[0].asInt64() != 1)
    {
        throw Exception("Reliable message store: local AOF fsync was not confirmed");
    }
}

ReliableMessageStore::Transaction::Transaction(ReliableMessageStore& store)
    : m_store(store), m_lock(store.m_mutex, defer_lock)
{
    if (s_current)
    {
        throw Exception("Nested reliable publication transaction");
    }
    m_lock.lock();
    if (store.m_indeterminate)
    {
        throw Exception("Reliable message store: indeterminate commit; restart to recover persistent sessions");
    }
    s_current = this;
}

ReliableMessageStore::Transaction::~Transaction()
{
    s_current = nullptr;
    if (!m_applied)
    {
        for (const auto& action: m_rollback)
        {
            action();
        }
    }
}

void ReliableMessageStore::Transaction::set(const string& key, const Buffer& value)
{
    m_operations.push_back({"SET", key, "", string(value.c_str(), value.bytes())});
}

void ReliableMessageStore::Transaction::hashSet(const string& key, const string& field, const Buffer& value)
{
    m_operations.push_back({"HSET", key, field, string(value.c_str(), value.bytes())});
}

void ReliableMessageStore::Transaction::addMember(const string& key, const string& member)
{
    m_operations.push_back({"SADD", key, member, ""});
}

void ReliableMessageStore::Transaction::hashDelete(const string& key, const string& field)
{
    m_operations.push_back({"HDEL", key, field, ""});
}

void ReliableMessageStore::Transaction::onCommit(function<void()> action)
{
    m_commit.push_back(std::move(action));
}

void ReliableMessageStore::Transaction::onRollback(function<void()> action)
{
    m_rollback.push_back(std::move(action));
}

void ReliableMessageStore::Transaction::commit()
{
    if (m_applied || s_current != this)
    {
        throw Exception("Reliable publication transaction is no longer active");
    }
    RedisCommand command("EVAL", Commit);
    command.emplace_back(to_string(m_operations.size()));
    for (const auto& operation: m_operations)
    {
        command.emplace_back(operation.key);
    }
    for (const auto& operation: m_operations)
    {
        command.emplace_back(operation.kind);
        command.emplace_back(operation.field);
        command.emplace_back(operation.value);
    }
    vector<Variant> result;
    try
    {
        m_store.m_redis.executeCommand(command, result);
    }
    catch (...)
    {
        // A lost response cannot distinguish an applied script from a rejected one. Do not
        // accept another publish against stale in-memory QoS2 state; startup recovery reconciles it.
        m_store.m_indeterminate = true;
        throw;
    }
    if (result.size() != 1 || result[0].asInt64() != 1)
    {
        throw Exception("Reliable publication transaction failed");
    }
    m_applied = true;
    s_current = nullptr;
    exception_ptr failure;
    try { sync(m_store.m_redis); }
    catch (...) { failure = current_exception(); }
    // A timeout cannot undo a committed record. Install every committed delivery, including
    // after an fsync timeout, but withhold the publisher's successful ACK in that case.
    for (const auto& action: m_commit)
    {
        try { action(); }
        catch (...) { if (!failure) failure = current_exception(); }
    }
    if (failure) rethrow_exception(failure);
}
