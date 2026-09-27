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
        std::string m_payload; ///< Message payload; never empty, an empty payload clears instead.
        Qos         m_qos {Qos::Qos0};
    };

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

    /**
     * @brief Store a retained message, or clear the topic's message when the payload is empty.
     *
     * An empty payload clearing the retained message is MQTT's own rule, not a convention of ours.
     *
     * @param topicName         Concrete topic the message was published to; never a filter.
     * @param payload           Message payload.
     * @param qos               Quality of service the message was published with.
     * @return What changed, so the caller knows whether to write to storage and how to move the
     *         broker's retained-message counter.
     */
    Change set(std::string_view topicName, std::string_view payload, Qos qos);

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
     * @return Storage key holding this node's retained messages.
     */
    static std::string storageKey(const std::string& nodeId);

private:
    mutable std::shared_mutex               m_mutex;
    std::unordered_map<std::string, Record> m_messages;

    static sptk::Buffer pack(const Record& record);
    static bool         unpack(const sptk::Buffer& packed, Record& record);
};

} // namespace xmq
