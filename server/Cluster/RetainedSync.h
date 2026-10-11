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

#include "../Subscription/RetainedMessages.h"

#include <sptk5/Logger.h>
#include <sptk5/net/RedisConnect.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace xmq::cluster {

/**
 * @brief A cluster node's retained messages, kept in line with the shared storage.
 *
 * The storage is the authority. Retained messages are kept there in ShardCount hashes
 * (cluster:retained:<k>), each with a revision counter (cluster:retained_rev:<k>); a commit script
 * numbers a change with its shard's next revision and writes it - a clearing removes the topic - in
 * one atomic step, so no two changes share a revision and no later write can be overtaken by an
 * earlier one. Clearings leave nothing behind: a node that was away reads the shards again rather
 * than offering what it kept, and a change a node hears of for a topic it does not hold is checked
 * against the storage rather than believed, so a late one cannot bring a cleared message back.
 *
 * A change published on this node is shown to its clients at once and committed behind, by one
 * thread, in the order it was taken; once committed it goes to the other nodes with its revision.
 * The revisions of a shard leave no gaps, so a node that misses one - its committer died before
 * sending it, or the link was down - sees the counter run ahead of what it has, and reads the shard
 * again.
 */
class RetainedSync
{
public:
    /// Writes to the node's log.
    using Log = std::function<void(sptk::LogPriority priority, const sptk::Logger::OutputString& output)>;

    /// Sends a committed change to the other nodes.
    using Broadcast = std::function<void(const std::string& topicName, const RetainedMessages::Record& record)>;

    /**
     * @brief Constructor. Nothing happens until start().
     * @param redisUrl          The shared storage.
     * @param messages          This node's retained messages.
     * @param broadcast         Sends a committed change to the other nodes.
     * @param changed           Told of every change clients can see, for the broker's counter.
     * @param log               Writes to the node's log.
     */
    RetainedSync(sptk::URL redisUrl, RetainedMessages& messages, Broadcast broadcast, RetainedMessages::Changed changed,
                 Log log);

    ~RetainedSync();

    RetainedSync(const RetainedSync&) = delete;
    RetainedSync& operator=(const RetainedSync&) = delete;

    /**
     * @brief Read every shard from the storage, then keep the retained messages in line with it.
     *
     * The reading is done before this returns, so that a node serves no client before it holds
     * what the cluster retains. A storage that cannot be read now is read when it can be.
     */
    void start();

    /**
     * @brief Stop. Changes not committed yet are dropped, as a node that stops drops them.
     */
    void stop();

    /**
     * @brief A retained message published on this node, or a clearing.
     * @param topicName         Concrete topic.
     * @param payload           Payload; empty clears the topic.
     * @param qos               Quality of service it was published with.
     */
    void publish(const std::string& topicName, std::string_view payload, Qos qos);

    /**
     * @brief A committed change another node sent.
     * @param topicName         Concrete topic.
     * @param record            The change, with its revision.
     */
    void receive(const std::string& topicName, const RetainedMessages::Record& record);

    /**
     * @brief Read every shard again - after this node was cut off from the storage, and may have
     *        missed anything.
     */
    void resynchronize();

    /**
     * @brief For tests: commit changes but send none to the other nodes, as a node that dies
     *        between the two does.
     * @param lost              True to send nothing.
     */
    void simulateBroadcastLoss(bool lost)
    {
        m_broadcastLost = lost;
    }

    /// @return The storage key of a shard's retained messages.
    static std::string shardKey(size_t shard);

    /// @return The storage key of a shard's revision counter.
    static std::string counterKey(size_t shard);

private:
    /// A change published here, waiting for its commit.
    struct Commit
    {
        std::string m_topicName;
        std::string m_payload;
        Qos         m_qos;
    };

    /// What this node has applied of a shard: every revision up to m_contiguous, and some above.
    struct Shard
    {
        int64_t                                              m_contiguous {0};
        std::set<int64_t>                                    m_above;
        std::optional<std::chrono::steady_clock::time_point> m_gapSince;
    };

    const sptk::URL           m_redisUrl;
    RetainedMessages&         m_messages;
    Broadcast                 m_broadcast;
    RetainedMessages::Changed m_changed;
    Log                       m_log;

    std::mutex                                             m_mutex;      ///< Guards what follows, up to the thread.
    std::condition_variable                                m_wake;
    bool                                                   m_stopping {false};
    bool                                                   m_resynchronize {false};
    std::deque<Commit>                                     m_commits;
    std::deque<std::pair<std::string, RetainedMessages::Record>> m_received;
    std::thread                                            m_thread;

    std::atomic_bool                                       m_broadcastLost {false};
    sptk::RedisConnect                                     m_redis;      ///< The thread's own.
    std::array<Shard, RetainedMessages::ShardCount>        m_shards;     ///< The thread's own.
    std::chrono::steady_clock::time_point                  m_lastCheck;  ///< The thread's own.

    void run();
    void connect();
    void commit(const std::vector<Commit>& commits);
    void apply(const std::string& topicName, const RetainedMessages::Record& record);
    void checkShards();
    void readShard(size_t shard);
    void readAllShards();
    void noteRevision(size_t shard, int64_t revision);
    void reportChange(const std::string& topicName, RetainedMessages::Change change) const;
};

} // namespace xmq::cluster
