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

#include "base/Qos.h"
#include "base/Topic.h"
#include "storage/Storage.h"

#include <sptk5/Buffer.h>

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xmq {

/**
 * @brief Retained messages, keyed by the topic they were published to.
 *
 * A retained message belongs to a topic, not to a subscription: it has to survive with nobody
 * subscribed, it has to be handed to every later subscriber whose filter matches, and a publication
 * to one topic must not disturb the retained message of another. Keeping it on the Subscription
 * object - as this used to - broke all three: a publication to `a/b` also wrote into the wildcard
 * subscription `a/+`, where the next publication to `a/c` overwrote it, and a wildcard subscriber
 * was then given a single message under the filter's own name rather than one per matching topic.
 *
 * Storing them here also makes them persistable, which they were not: the subscription tree is
 * rebuilt from each session's subscription list on restart, so anything held on it was lost.
 */
class RetainedMessages final
{
public:
    /**
     * @brief A topic's retained message, or - with an empty payload - its clearing.
     *
     * In a cluster every change is committed in the shared storage, which numbers it with the next
     * revision of the topic's shard; the greatest committed revision is the topic's state on every
     * node. A node alone keeps revision 0: there is nobody to order its changes against.
     */
    struct Record
    {
        std::string m_payload;      ///< Message payload; empty when the change clears the topic.
        Qos         m_qos {Qos::Qos0};
        int64_t     m_revision {0}; ///< The committed revision; 0 when not committed - a node alone, or a change in flight.

        [[nodiscard]] bool clears() const
        {
            return m_payload.empty();
        }
    };

    /// The shards the shared storage keeps retained messages in, each with a revision counter of its own.
    static constexpr size_t ShardCount = 16;

    /// Called for each stored message matching a filter: its concrete topic name and the record.
    using Handler = std::function<void(const std::string& topicName, const Record& record)>;

    /// What a change did, so the caller can keep the broker's retained-message count right.
    enum class Change
    {
        None,     ///< Nothing a client can see changed.
        Added,    ///< This topic had no retained message before.
        Replaced, ///< An existing retained message was overwritten.
        Removed   ///< The topic's retained message was cleared.
    };

    /// Called for every topic whose retained message changed.
    using Changed = std::function<void(const std::string& topicName, Change change)>;

    /**
     * @brief A node alone: store a retained message published here, or clear the topic's message
     *        when the payload is empty - MQTT's own rule.
     * @param topicName         Concrete topic the message was published to; never a filter.
     * @param payload           Message payload.
     * @param qos               Quality of service the message was published with.
     * @return What changed. Anything but None has to be written to the node's storage.
     */
    Change set(std::string_view topicName, std::string_view payload, Qos qos);

    /**
     * @brief A node in a cluster: take a change published here, before the storage has committed it.
     *
     * The change is what this node's clients see at once, and its commit follows. Until the last
     * commit of the topic is back, changes from other nodes are only remembered, and the greatest
     * committed revision is what the topic shows when it is.
     *
     * @param topicName         Concrete topic.
     * @param payload           Message payload; empty clears the topic.
     * @param qos               Quality of service the message was published with.
     * @return What changed.
     */
    Change takeLocal(std::string_view topicName, std::string_view payload, Qos qos);

    /**
     * @brief The storage committed a change this node took, and numbered it.
     * @param topicName         Concrete topic.
     * @param committed         The change, with its revision.
     * @return What changed.
     */
    Change applyCommit(std::string_view topicName, const Record& committed);

    /**
     * @brief Apply a committed change made elsewhere: sent by another node, or read from the storage.
     *
     * Taken only when its revision is greater than the topic's here; an equal one is a repeat, a
     * lower one is late. A change to a topic this node does not hold cannot be told from a late one -
     * a clearing after it leaves nothing behind - so it is taken only from the storage itself, and
     * otherwise reported as unknown, to be read from there.
     *
     * @param topicName         Concrete topic.
     * @param committed         The change, with its revision.
     * @param fromStorage       Read from the storage, which is the authority.
     * @param unknown           Set when the topic is not held here and the change has to be checked.
     * @return What changed.
     */
    Change applyRemote(std::string_view topicName, const Record& committed, bool fromStorage, bool& unknown);

    /**
     * @brief Bring one shard in line with the storage.
     *
     * Every record the storage holds is applied as a committed change. A topic held here and absent
     * there was cleared - unless its revision is above what the storage had numbered when it was
     * read, a change committed since, or a change of this node's is still in flight.
     *
     * @param shard             The shard.
     * @param numbered          The shard's revision counter, read before its records.
     * @param stored            The shard's records, by topic.
     * @param changed           Called for every topic whose retained message changed.
     */
    void reconcile(size_t shard, int64_t numbered, const std::unordered_map<std::string, Record>& stored,
                   const Changed& changed);

    /**
     * @param topicName         Concrete topic.
     * @return The shard holding the topic: the same on every node and every platform.
     */
    static size_t shardOf(std::string_view topicName);

    /**
     * @brief A record as the shared storage holds it: "<revision>:<qos>:<payload>", which the commit
     *        script writes.
     * @param stored            The stored value.
     * @param record            Receives the record.
     * @return false when the value is damaged.
     */
    static bool parseShared(std::string_view stored, Record& record);

    /**
     * @brief Append a record to a cluster message.
     * @param message           Message being built.
     * @param topicName         Topic of the record.
     * @param record            Record to append.
     */
    static void encode(sptk::Buffer& message, std::string_view topicName, const Record& record);

    /**
     * @brief Read the records of a cluster message.
     * @param message           Message built by encode().
     * @param handler           Called once per record.
     * @return false when the message is damaged; the records before the damage were handed over.
     */
    static bool decode(std::string_view message, const Handler& handler);

    /**
     * @brief Call the handler for every stored message whose topic matches the filter.
     * @param filter            Subscription filter, with or without wildcards.
     * @param handler           Called once per matching topic.
     */
    void forEachMatching(std::string_view filter, const Handler& handler) const;

    /**
     * @return Number of topics currently holding a retained message.
     */
    [[nodiscard]] size_t size() const;

    /**
     * @brief Does a concrete topic match a subscription filter?
     *
     * Public because it is worth testing on its own: '+' takes exactly one level, '#' takes the
     * rest and must come last, and neither may match a topic starting with '$' when it is the
     * filter's first level - a client asking for '#' is not asking for the broker's own metrics.
     */
    static bool matches(std::string_view filter, std::string_view topicName);

    /**
     * @brief A node alone: read every retained message back from its storage.
     *
     * Call before any listener exists, for the same reason session restore does: a client that
     * subscribes in the meantime would be told there is nothing retained.
     */
    void load(const SStorage& storage, const std::string& nodeId);

    /**
     * @brief A node alone: write one topic's retained message to its storage, or remove it when cleared.
     *
     * Called only when a retained message is set or cleared, so ordinary publishing costs nothing.
     */
    void store(const SStorage& storage, const std::string& nodeId, const std::string& topicName) const;

    /**
     * @brief A record as it is written to a node's own storage.
     * @param record            Record to write.
     * @return the stored form.
     */
    static sptk::Buffer pack(const Record& record);

    /**
     * @brief A record read back from a node's own storage, in this format or the one before records
     *        carried a revision.
     * @param packed            Stored form.
     * @param record            Receives the record.
     * @return false when the stored form is damaged.
     */
    static bool unpack(const sptk::Buffer& packed, Record& record);

    /**
     * @return Storage key holding a node's own retained messages, when it is not in a cluster.
     */
    static std::string storageKey(const std::string& nodeId);

private:
    /**
     * @brief What is held for a topic.
     *
     * A message, or - only while a clearing made here is in flight - nothing, kept to count the
     * commits. m_best is the greatest committed state heard of while changes of this node's were in
     * flight, and the topic shows it when the last of them is back.
     */
    struct Entry
    {
        Record m_record;          ///< What clients see; its revision is committed when nothing is in flight.
        bool   m_present {false}; ///< m_record is a message, not a clearing in flight.
        int    m_inFlight {0};    ///< Changes of this node's not committed yet.
        Record m_best;            ///< The greatest committed state while changes are in flight.
    };

    /// Show a committed state; returns what clients see change. Erases nothing.
    static Change show(Entry& entry, const Record& committed);

    mutable std::shared_mutex              m_mutex;
    std::unordered_map<std::string, Entry> m_messages; ///< Present messages, and clearings in flight.
};

} // namespace xmq
