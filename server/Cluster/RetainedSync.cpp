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

#include "RetainedSync.h"

#include <sptk5/net/RedisCommand.h>

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace xmq::cluster;

namespace {

/// How often the shard counters are compared with what this node holds.
constexpr auto CheckInterval = 500ms;

/// How long a shard may stay behind its counter before it is read again. A change committed on
/// another node is normally here within milliseconds; one still missing after this was not sent.
constexpr auto GapTimeout = 1s;

/// How long a storage command may take. The coordinator takes the node offline when the storage
/// stops answering; this thread only must not hang on it.
constexpr auto ReadTimeout = 2s;

/// The most changes one commit takes. Commits are one round trip each, so a burst of retained
/// changes goes in a few of them.
constexpr size_t MaxCommitBatch = 256;

/**
 * Commits a batch of changes. For each one, KEYS holds its shard's counter and hash, and ARGV its
 * topic, QoS, payload and whether it clears. The counter is incremented and the record written or
 * removed in the same atomic step; the revisions come back in order.
 */
constexpr auto CommitScript = R"(
local revisions = {}
for i = 1, #KEYS / 2 do
    local revision = redis.call('INCR', KEYS[2 * i - 1])
    local base = (i - 1) * 4
    if ARGV[base + 4] == '1' then
        redis.call('HDEL', KEYS[2 * i], ARGV[base + 1])
    else
        redis.call('HSET', KEYS[2 * i], ARGV[base + 1], revision .. ':' .. ARGV[base + 2] .. ':' .. ARGV[base + 3])
    end
    revisions[i] = revision
end
return revisions
)";

int64_t counterValue(const Variant& value)
{
    if (value.isNull())
    {
        return 0;
    }
    const string text = value.asString().c_str();
    return text.empty() ? 0 : stoll(text);
}

} // namespace

RetainedSync::RetainedSync(URL redisUrl, RetainedMessages& messages, Broadcast broadcast, RetainedMessages::Changed changed,
                           Log log)
    : m_redisUrl(std::move(redisUrl))
    , m_messages(messages)
    , m_broadcast(std::move(broadcast))
    , m_changed(std::move(changed))
    , m_log(std::move(log))
{
}

RetainedSync::~RetainedSync()
{
    stop();
}

string RetainedSync::shardKey(const size_t shard)
{
    return "cluster:retained:" + to_string(shard);
}

string RetainedSync::counterKey(const size_t shard)
{
    return "cluster:retained_rev:" + to_string(shard);
}

void RetainedSync::start()
{
    const scoped_lock lock(m_mutex);
    if (m_thread.joinable())
    {
        return;
    }
    m_stopping = false;
    try
    {
        readAllShards();
    }
    catch (const Exception& e)
    {
        m_log(LogPriority::Warning, [&e] { return format("Can't read the retained messages yet: {}", e.what()); });
        m_redis.disconnect();
        m_resynchronize = true;
    }
    m_lastCheck = chrono::steady_clock::now();
    m_thread = thread([this] { run(); });
}

void RetainedSync::stop()
{
    {
        const scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void RetainedSync::publish(const string& topicName, const string_view payload, const Qos qos)
{
    RetainedMessages::Change change;
    {
        // Taken and queued under one lock, so that changes are committed in the order clients see them.
        const scoped_lock lock(m_mutex);
        change = m_messages.takeLocal(topicName, payload, qos);
        m_commits.push_back({topicName, string(payload), qos});
    }
    m_wake.notify_one();
    reportChange(topicName, change);
}

void RetainedSync::receive(const string& topicName, const RetainedMessages::Record& record)
{
    {
        const scoped_lock lock(m_mutex);
        m_received.emplace_back(topicName, record);
    }
    m_wake.notify_one();
}

void RetainedSync::resynchronize()
{
    {
        const scoped_lock lock(m_mutex);
        m_resynchronize = true;
    }
    m_wake.notify_one();
}

void RetainedSync::run()
{
    while (true)
    {
        vector<Commit>                                     commits;
        deque<pair<string, RetainedMessages::Record>>      received;
        bool                                               resynchronize = false;
        {
            unique_lock lock(m_mutex);
            m_wake.wait_for(lock, CheckInterval, [this]
                            {
                                return m_stopping || m_resynchronize || !m_commits.empty() || !m_received.empty();
                            });
            if (m_stopping)
            {
                // What was queued before the stop is what publishers were answered for: a retained
                // message set and acknowledged must be in the storage once the server is gone, or a
                // restart brings the broker back without it. Dropping the queue here is how
                // Retained_SurvivesServerRestart lost its message on a busy machine - the test
                // waited a hundred milliseconds for the write and the stop came first.
                commits.assign(m_commits.begin(), m_commits.end());
                m_commits.clear();
                if (!commits.empty())
                {
                    try
                    {
                        commit(commits);
                    }
                    catch (const sptk::Exception&)
                    {
                        // Nothing left to tell: the storage is going away with this node.
                    }
                }
                return;
            }
            // Commits stay queued until they are done: one that fails is tried again, in its place.
            for (size_t i = 0; i < m_commits.size() && i < MaxCommitBatch; ++i)
            {
                commits.push_back(m_commits[i]);
            }
            received.swap(m_received);
            resynchronize = m_resynchronize;
            m_resynchronize = false;
        }

        try
        {
            if (resynchronize)
            {
                readAllShards();
            }
            if (!commits.empty())
            {
                commit(commits);
                const scoped_lock lock(m_mutex);
                m_commits.erase(m_commits.begin(), m_commits.begin() + static_cast<ptrdiff_t>(commits.size()));
            }
            while (!received.empty())
            {
                apply(received.front().first, received.front().second);
                received.pop_front();
            }
            if (chrono::steady_clock::now() - m_lastCheck >= CheckInterval)
            {
                checkShards();
                m_lastCheck = chrono::steady_clock::now();
            }
        }
        catch (const Exception& e)
        {
            m_log(LogPriority::Warning, [&e] { return format("Retained messages: the storage failed: {}", e.what()); });
            m_redis.disconnect();
            {
                // What was not done is done when the storage is back: the changes received are put
                // back in front, and the shards read again, in case anything went by meanwhile.
                const scoped_lock lock(m_mutex);
                m_received.insert(m_received.begin(), received.begin(), received.end());
                m_resynchronize = true;
            }
            unique_lock lock(m_mutex);
            m_wake.wait_for(lock, CheckInterval, [this] { return m_stopping; });
        }
    }
}

void RetainedSync::connect()
{
    if (m_redis.isConnected())
    {
        return;
    }
    m_redis.setReadTimeout(ReadTimeout);
    const auto& [host, port] = m_redisUrl.hostAndPort();
    m_redis.connect(host, port, m_redisUrl.username(), m_redisUrl.password(), m_redisUrl.path(),
                    chrono::duration_cast<chrono::milliseconds>(ReadTimeout));
}

void RetainedSync::commit(const vector<Commit>& commits)
{
    connect();

    vector<string> keys;
    vector<string> arguments;
    for (const auto& commit: commits)
    {
        const auto shard = RetainedMessages::shardOf(commit.m_topicName);
        keys.push_back(counterKey(shard));
        keys.push_back(shardKey(shard));
        arguments.push_back(commit.m_topicName);
        arguments.push_back(to_string(static_cast<int>(commit.m_qos)));
        arguments.push_back(commit.m_payload);
        arguments.emplace_back(commit.m_payload.empty() ? "1" : "0");
    }

    RedisCommand command("EVAL", CommitScript);
    command.emplace_back(to_string(keys.size()));
    command.emplace_back(keys);
    command.emplace_back(arguments);
    vector<Variant> revisions;
    m_redis.executeCommand(command, revisions);
    if (revisions.size() != commits.size())
    {
        throw Exception(format("The commit returned {} revision(s) for {} change(s)", revisions.size(), commits.size()));
    }

    for (size_t i = 0; i < commits.size(); ++i)
    {
        const auto&                   commit = commits[i];
        const RetainedMessages::Record record {commit.m_payload, commit.m_qos, revisions[i].asInt64()};
        reportChange(commit.m_topicName, m_messages.applyCommit(commit.m_topicName, record));
        noteRevision(RetainedMessages::shardOf(commit.m_topicName), record.m_revision);
        if (!m_broadcastLost)
        {
            m_broadcast(commit.m_topicName, record);
        }
    }
}

void RetainedSync::apply(const string& topicName, const RetainedMessages::Record& record)
{
    const auto shard = RetainedMessages::shardOf(topicName);
    bool       unknown = false;
    reportChange(topicName, m_messages.applyRemote(topicName, record, false, unknown));
    if (unknown)
    {
        // Not held here: a late change to a topic cleared since would look the same. The storage
        // says which it is - and if the topic has changed again, it says what it holds now.
        connect();
        const auto stored = m_redis.getHashValue(shardKey(shard), topicName);
        if (RetainedMessages::Record current; !stored.isNull() && RetainedMessages::parseShared(stored.asString().c_str(), current))
        {
            reportChange(topicName, m_messages.applyRemote(topicName, current, true, unknown));
        }
    }
    noteRevision(shard, record.m_revision);
}

void RetainedSync::checkShards()
{
    connect();

    vector<string> keys;
    for (size_t shard = 0; shard < RetainedMessages::ShardCount; ++shard)
    {
        keys.push_back(counterKey(shard));
    }
    const auto counters = m_redis.getValues(keys);
    const auto now = chrono::steady_clock::now();

    for (size_t shard = 0; shard < RetainedMessages::ShardCount; ++shard)
    {
        const auto counter = counters.find(keys[shard]);
        const auto numbered = counter == counters.end() ? 0 : counterValue(counter->second);
        auto&      state = m_shards[shard];
        if (numbered <= state.m_contiguous)
        {
            state.m_gapSince.reset();
            continue;
        }
        // Behind the counter: changes on their way here, or changes never sent. Given the time the
        // first need, the shard is read again.
        if (!state.m_gapSince)
        {
            state.m_gapSince = now;
        }
        else if (now - *state.m_gapSince >= GapTimeout)
        {
            m_log(LogPriority::Debug, [shard, numbered, &state]
                       {
                           return format("Retained shard {} is at revision {} here and {} in the storage: reading it again.",
                                         shard, state.m_contiguous, numbered);
                       });
            readShard(shard);
        }
    }
}

void RetainedSync::readShard(const size_t shard)
{
    connect();

    // The counter first: whatever the records then show was numbered no later than it, and anything
    // held here above it was committed since and stays.
    const auto numbered = counterValue(m_redis.getValue(counterKey(shard)));

    unordered_map<string, RetainedMessages::Record> stored;
    for (const auto& [topicName, value]: m_redis.getHashValues(shardKey(shard)))
    {
        if (RetainedMessages::Record record; RetainedMessages::parseShared(value.asString().c_str(), record))
        {
            stored.emplace(topicName, std::move(record));
        }
    }

    m_messages.reconcile(shard, numbered, stored, [this](const string& topicName, const RetainedMessages::Change change)
                         {
                             reportChange(topicName, change);
                         });

    auto& state = m_shards[shard];
    state.m_contiguous = max(state.m_contiguous, numbered);
    state.m_above.erase(state.m_above.begin(), state.m_above.upper_bound(state.m_contiguous));
    noteRevision(shard, state.m_contiguous);
    state.m_gapSince.reset();
}

void RetainedSync::readAllShards()
{
    for (size_t shard = 0; shard < RetainedMessages::ShardCount; ++shard)
    {
        readShard(shard);
    }
}

void RetainedSync::noteRevision(const size_t shard, const int64_t revision)
{
    auto& state = m_shards[shard];
    if (revision > state.m_contiguous)
    {
        state.m_above.insert(revision);
    }
    while (!state.m_above.empty() && *state.m_above.begin() <= state.m_contiguous + 1)
    {
        state.m_contiguous = max(state.m_contiguous, *state.m_above.begin());
        state.m_above.erase(state.m_above.begin());
    }
}

void RetainedSync::reportChange(const string& topicName, const RetainedMessages::Change change) const
{
    if (change != RetainedMessages::Change::None && m_changed)
    {
        m_changed(topicName, change);
    }
}
