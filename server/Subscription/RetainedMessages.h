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
     * @brief What is kept for a topic.
     */
    struct Record
    {
        std::string m_payload;     ///< Message payload; empty only in a tombstone.
        Qos         m_qos {Qos::Qos0};
        int64_t     m_updated {0}; ///< When it was set or cleared, in cluster-clock milliseconds; 0 when unknown.

        /**
         * @brief Is this the trace of a cleared message rather than a message?
         *
         * A cleared topic keeps its clearing time for a while. Without it a node that missed the
         * clearing - restarted, or cut off - would hand its old copy back to the cluster on rejoining,
         * and nothing could tell the old copy from a new one.
         */
        [[nodiscard]] bool isTombstone() const
        {
            return m_payload.empty();
        }

        /**
         * @brief Does this record supersede another for the same topic?
         *
         * The later change wins. Two changes made in the same millisecond on different nodes are
         * ordered by their content, so that every node picks the same one.
         */
        [[nodiscard]] bool supersedes(const Record& other) const
        {
            if (m_updated != other.m_updated)
            {
                return m_updated > other.m_updated;
            }
            if (m_payload != other.m_payload)
            {
                return m_payload > other.m_payload;
            }
            return m_qos > other.m_qos;
        }
    };

    /// How long a tombstone is kept. A node that rejoins after longer may bring a cleared message back.
    static constexpr int64_t TombstoneLifetimeMs = 24 * 60 * 60 * 1000;

    /// Called for each stored message matching a filter: its concrete topic name and the record.
    using Handler = std::function<void(const std::string& topicName, const Record& record)>;

    /// What a set() call did, so the caller can keep the broker's retained-message count right.
    enum class Change
    {
        None,     ///< The same message was already stored.
        Added,    ///< This topic had no retained message before.
        Replaced, ///< An existing retained message was overwritten.
        Removed   ///< An empty payload cleared the topic's retained message.
    };

    /// What applying a change did.
    struct Applied
    {
        Change change {Change::None}; ///< Effect on the retained messages, for the broker's counter.
        bool   stored {false};        ///< The record changed and has to be written to storage.
        Record record;                ///< The topic's record afterwards.
    };

    /**
     * @brief Store a retained message published on this node, or clear the topic's message when
     *        the payload is empty.
     *
     * An empty payload clearing the retained message is MQTT's own rule, not a convention of ours.
     * A publication on this node always takes effect: it is stamped later than whatever it replaces,
     * even when that came from a node whose clock runs ahead.
     *
     * @param topicName         Concrete topic the message was published to; never a filter.
     * @param payload           Message payload.
     * @param qos               Quality of service the message was published with.
     * @param now               Current cluster time, in milliseconds.
     * @return What changed. When stored is true the change is also news to the other cluster nodes.
     */
    Applied set(std::string_view topicName, std::string_view payload, Qos qos, int64_t now = 0);

    /**
     * @brief Apply a change made on another cluster node, if it is newer than what is held here.
     * @param topicName         Concrete topic.
     * @param record            The other node's record; a tombstone clears.
     * @return What changed; nothing when the record held here is as new or newer.
     */
    Applied merge(std::string_view topicName, const Record& record);

    /**
     * @brief Drop the tombstones older than their lifetime.
     * @param now               Current cluster time, in milliseconds.
     * @return Topics whose tombstones were dropped, to be removed from storage too.
     */
    std::vector<std::string> purgeTombstones(int64_t now);

    /**
     * @brief Call the handler for every record, tombstones included.
     *
     * This is what a node sends a peer that joins, so the peer learns the clearings as well.
     *
     * @param handler           Called once per topic.
     */
    void forEachRecord(const Handler& handler) const;

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
     *
     * Tombstones are not messages, and are skipped.
     *
     * @param filter            Subscription filter, with or without wildcards.
     * @param handler           Called once per matching topic.
     */
    void forEachMatching(std::string_view filter, const Handler& handler) const;

    /**
     * @return Number of topics currently holding a retained message; tombstones do not count.
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
     * @brief Read every retained message back from storage.
     *
     * Call before any listener exists, for the same reason session restore does: a client that
     * subscribes in the meantime would be told there is nothing retained.
     */
    void load(const SStorage& storage, const std::string& nodeId);

    /**
     * @brief Write one topic's retained message to storage, or remove it when cleared.
     *
     * Called only when a retained message is set or cleared, so ordinary publishing costs nothing.
     */
    void store(const SStorage& storage, const std::string& nodeId, const std::string& topicName) const;

    /**
     * @brief A record as it is written to storage.
     * @param record            Record to write.
     * @return the stored form.
     */
    static sptk::Buffer pack(const Record& record);

    /**
     * @brief A record read back from storage, in this format or the one before records carried a
     *        time.
     * @param packed            Stored form.
     * @param record            Receives the record.
     * @return false when the stored form is damaged.
     */
    static bool unpack(const sptk::Buffer& packed, Record& record);

    /**
     * @return Storage key holding this node's retained messages.
     */
    static std::string storageKey(const std::string& nodeId);

private:
    mutable std::shared_mutex               m_mutex;
    std::unordered_map<std::string, Record> m_messages; ///< Messages and tombstones.

};

} // namespace xmq
