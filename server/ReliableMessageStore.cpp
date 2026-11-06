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
    local firstKeyUse = not expectedTypes[KEYS[i]]
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
    if firstKeyUse then
        local actual = redis.call('TYPE', KEYS[i]).ok
        if actual ~= 'none' and actual ~= expected then
            return redis.error_reply('wrong publication key type')
        end
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
    m_worker = thread([this] { run(); });
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

ReliableMessageStore::~ReliableMessageStore()
{
    shutdown();
}

void ReliableMessageStore::drain()
{
    if (this_thread::get_id() == m_worker.get_id())
        throw Exception("Reentrant reliable journal fence");
    unique_lock lock(m_queueMutex);
    const auto target = m_finished + m_outstanding;
    m_changed.wait(lock, [this, target] { return m_finished >= target; });
}

void ReliableMessageStore::shutdown()
{
    {
        lock_guard lock(m_queueMutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

ReliableMessageStore::Statistics ReliableMessageStore::statistics() const
{
    lock_guard lock(m_queueMutex);
    return {m_transactions, m_batches, m_largestBatch};
}

ReliableMessageStore::Transaction::Transaction(ReliableMessageStore& store, const bool readsRetained)
    : m_store(store), m_lock(store.m_mutex, defer_lock)
{
    if (s_current || this_thread::get_id() == store.m_worker.get_id())
        throw Exception("Nested reliable publication transaction");
    m_lock.lock();
    // A SUBSCRIBE after an accepted retained publication must replay its latest value.
    // Ordinary PUBLISH staging can overlap I/O; a retained reader fences pending changes.
    if (readsRetained)
    {
        bool pending;
        { lock_guard lock(store.m_queueMutex); pending = store.m_pendingRetained != 0; }
        if (pending) store.drain();
    }
    if (store.m_indeterminate)
        throw Exception("Reliable message store: indeterminate commit; restart to recover persistent sessions");
    s_current = this;
}

ReliableMessageStore::Transaction::~Transaction()
{
    if (s_current == this) s_current = nullptr;
    if (!m_submitted)
    {
        for (const auto& action: m_rollback)
        {
            try { action(); }
            catch (...) { m_store.invalidate(); }
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
    m_commit.push_back({std::move(action), false});
}

void ReliableMessageStore::Transaction::onDurable(function<void()> action)
{
    m_commit.push_back({std::move(action), true});
}

void ReliableMessageStore::Transaction::onRollback(function<void()> action)
{
    m_rollback.push_back(std::move(action));
}

std::shared_ptr<ReliableMessageStore::Pending> ReliableMessageStore::Transaction::submit(
    function<void()> success, function<void(exception_ptr)> failure)
{
    if (m_submitted || s_current != this)
        throw Exception("Reliable publication transaction is no longer active");
    auto pending = make_shared<Pending>();
    pending->operations = move(m_operations);
    pending->commit = move(m_commit);
    pending->rollback = move(m_rollback);
    pending->success = move(success);
    pending->failure = move(failure);
    pending->changesRetained = m_changesRetained;
    pending->bytes = sizeof(Pending);
    for (const auto& op: pending->operations)
        pending->bytes += sizeof(Operation) + op.kind.size() + op.key.size() + op.field.size() + op.value.size();
    try { m_store.enqueue(pending); }
    catch (...)
    {
        m_operations = move(pending->operations);
        m_commit = move(pending->commit);
        m_rollback = move(pending->rollback);
        throw;
    }
    m_submitted = true;
    s_current = nullptr;
    m_lock.unlock();
    return pending;
}

void ReliableMessageStore::Transaction::commit()
{
    submit()->completed.get_future().get();
}

void ReliableMessageStore::Transaction::commitAsync(function<void()> success, function<void(exception_ptr)> failure)
{
    (void) submit(move(success), move(failure));
}

void ReliableMessageStore::enqueue(const shared_ptr<Pending>& pending)
{
    constexpr size_t MaxPending = 16384;
    constexpr size_t MaxBytes = 64 * 1024 * 1024;
    {
        lock_guard lock(m_queueMutex);
        if (m_stopping) throw Exception("Reliable message store is stopping");
        if (m_indeterminate) throw Exception("Reliable message store requires restart");
        if (m_outstanding >= MaxPending || pending->bytes > MaxBytes - m_pendingBytes)
            throw Exception("Reliable message store commit queue is full");
        m_pending.push_back(pending);
        ++m_outstanding;
        m_pendingBytes += pending->bytes;
        if (pending->changesRetained) ++m_pendingRetained;
    }
    m_changed.notify_one();
}

void ReliableMessageStore::eraseDelivery(string key, string field, function<void()> success,
                                          function<void(exception_ptr)> failure)
{
    queueDeliveryOperation({"HDEL", move(key), move(field), ""}, move(success), move(failure));
}

void ReliableMessageStore::writeDelivery(string key, string field, const Buffer& value,
                                         function<void()> success, function<void(exception_ptr)> failure)
{
    queueDeliveryOperation({"HSET", move(key), move(field), string(value.c_str(), value.bytes())}, move(success), move(failure));
}

void ReliableMessageStore::queueDeliveryOperation(Operation operation, function<void()> success,
                                                   function<void(exception_ptr)> failure)
{
    auto pending = make_shared<Pending>();
    pending->bytes = sizeof(Pending) + sizeof(Operation) + operation.kind.size() + operation.key.size() + operation.field.size() + operation.value.size();
    pending->operations.push_back(move(operation));
    pending->success = move(success);
    pending->failure = move(failure);
    enqueue(pending);
}

void ReliableMessageStore::run()
{
    constexpr size_t MaxBatch = 1024;
    constexpr size_t BatchBytes = 4 * 1024 * 1024;
    while (true)
    {
        vector<shared_ptr<Pending>> batch;
        {
            unique_lock lock(m_queueMutex);
            m_changed.wait(lock, [this] { return m_stopping || !m_pending.empty(); });
            if (m_pending.empty() && m_stopping) return;
            // At idle, collect a short burst. Under load the previous fsync already collected it.
            if (m_pending.size() < MaxBatch && !m_stopping)
                m_changed.wait_for(lock, chrono::microseconds(200), [this] { return m_stopping || m_pending.size() >= MaxBatch; });
            size_t bytes = 0;
            while (!m_pending.empty() && batch.size() < MaxBatch && (batch.empty() || bytes + m_pending.front()->bytes <= BatchBytes))
            {
                bytes += m_pending.front()->bytes;
                batch.push_back(move(m_pending.front()));
                m_pending.pop_front();
            }
        }
        exception_ptr failure;
        bool applied = false;
        bool durable = false;
        try
        {
            if (m_indeterminate) throw Exception("Reliable message store requires restart");
            size_t operations = 0;
            for (const auto& request: batch) operations += request->operations.size();
            RedisCommand command("EVAL", Commit);
            command.emplace_back(to_string(operations));
            for (const auto& request: batch)
                for (const auto& op: request->operations) command.emplace_back(op.key);
            for (const auto& request: batch)
                for (const auto& op: request->operations)
                {
                    command.emplace_back(op.kind);
                    command.emplace_back(op.field);
                    command.emplace_back(op.value);
                }
            vector<Variant> result;
            m_redis.executeCommand(command, result);
            if (result.size() != 1 || result[0].asInt64() != 1)
                throw Exception("Reliable publication batch failed");
            applied = true;
            sync(m_redis); // Same connection, one durable barrier for every record in the batch.
            durable = true;
        }
        catch (...) { failure = current_exception(); m_indeterminate = true; }

        // A successful script cannot be undone after a fence timeout. Install those records,
        // but never send success ACKs. Pre-write failures instead cancel staged reservations.
        const auto invoke = [&](const function<void()>& action)
        {
            try { action(); }
            catch (...) { if (!failure) failure = current_exception(); m_indeterminate = true; }
        };
        for (const auto& request: batch)
        {
            if (applied)
            {
                for (const auto& action: request->commit)
                    if (!action.durableOnly || durable) invoke(action.run);
            }
            else for (const auto& action: request->rollback) invoke(action);
        }

        for (const auto& request: batch)
        {
            try
            {
                if (failure)
                {
                    if (request->failure) request->failure(failure);
                    request->completed.set_exception(failure);
                }
                else
                {
                    if (request->success) request->success();
                    request->completed.set_value();
                }
            }
            catch (...)
            {
                // A completion callback must not terminate the journal writer or strand waiters.
                try { request->completed.set_exception(current_exception()); } catch (...) {}
            }
        }
        {
            lock_guard lock(m_queueMutex);
            if (durable)
            {
                m_transactions += batch.size();
                ++m_batches;
                m_largestBatch = max<uint64_t>(m_largestBatch, batch.size());
            }
            for (const auto& request: batch)
            {
                --m_outstanding;
                m_pendingBytes -= request->bytes;
                if (request->changesRetained) --m_pendingRetained;
            }
            m_finished += batch.size();
        }
        m_changed.notify_all();
    }
}
