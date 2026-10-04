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


#include "RetainedMessages.h"

#include <sptk5/Printer.h>

#include <algorithm>
#include <bit>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Split a topic or filter into levels. MQTT keeps empty levels significant: "a//b" has three.
vector<string_view> splitLevels(const string_view path)
{
    vector<string_view> levels;
    size_t              start = 0;
    while (true)
    {
        const auto separator = path.find('/', start);
        if (separator == string_view::npos)
        {
            levels.push_back(path.substr(start));
            break;
        }
        levels.push_back(path.substr(start, separator - start));
        start = separator + 1;
    }
    return levels;
}

} // namespace

bool RetainedMessages::matches(const string_view filter, const string_view topicName)
{
    const auto filterLevels = splitLevels(filter);
    const auto topicLevels = splitLevels(topicName);

    // A leading wildcard never reaches the broker's own topics: subscribing to '#' asks for
    // everything the clients publish, not for $SYS.
    if (!topicLevels.empty() && topicLevels.front().starts_with('$') &&
        !filterLevels.empty() && (filterLevels.front() == "#" || filterLevels.front() == "+"))
    {
        return false;
    }

    size_t level = 0;
    for (; level < filterLevels.size(); ++level)
    {
        const auto& filterLevel = filterLevels[level];

        if (filterLevel == "#")
        {
            // '#' is only legal last, and matches the remaining levels including none of them.
            return level + 1 == filterLevels.size();
        }

        if (level >= topicLevels.size())
        {
            return false;
        }

        if (filterLevel != "+" && filterLevel != topicLevels[level])
        {
            return false;
        }
    }

    return level == topicLevels.size();
}

RetainedMessages::Applied RetainedMessages::set(const string_view topicName, const string_view payload, const Qos qos,
                                                const int64_t now)
{
    const unique_lock lock(m_mutex);

    const auto [message, inserted] = m_messages.try_emplace(string(topicName));
    auto&      record = message->second;
    const auto wasMessage = !inserted && !record.isTombstone();

    Applied applied;
    if (payload.empty())
    {
        if (!inserted && record.isTombstone())
        {
            // Cleared already. Clearing it again tells nobody anything.
            applied.record = record;
            return applied;
        }
        // A clearing is recorded even when nothing was held here: another node may hold the
        // message, or receive it late, and the tombstone is what tells it the message is gone.
        applied.change = wasMessage ? Change::Removed : Change::None;
    }
    else
    {
        if (wasMessage && record.m_payload == payload && record.m_qos == qos)
        {
            applied.record = record;
            return applied;
        }
        applied.change = wasMessage ? Change::Replaced : Change::Added;
    }

    // Later than what it replaces, whatever the clock says: a node whose clock runs ahead must not
    // make the next publication here lose to the one before it.
    record.m_updated = inserted ? now : max(now, record.m_updated + 1);
    record.m_payload = payload;
    record.m_qos = qos;
    applied.stored = true;
    applied.record = record;
    return applied;
}

RetainedMessages::Applied RetainedMessages::merge(const string_view topicName, const Record& incoming)
{
    const unique_lock lock(m_mutex);

    const auto [message, inserted] = m_messages.try_emplace(string(topicName));
    auto&      record = message->second;

    Applied applied;
    if (!inserted && !incoming.supersedes(record))
    {
        applied.record = record;
        return applied;
    }

    const auto wasMessage = !inserted && !record.isTombstone();
    if (incoming.isTombstone())
    {
        applied.change = wasMessage ? Change::Removed : Change::None;
    }
    else
    {
        applied.change = wasMessage ? Change::Replaced : Change::Added;
    }

    record = incoming;
    applied.stored = true;
    applied.record = record;
    return applied;
}

vector<string> RetainedMessages::purgeTombstones(const int64_t now)
{
    vector<string> purged;

    const unique_lock lock(m_mutex);
    for (auto message = m_messages.begin(); message != m_messages.end();)
    {
        if (message->second.isTombstone() && now - message->second.m_updated > TombstoneLifetimeMs)
        {
            purged.push_back(message->first);
            message = m_messages.erase(message);
        }
        else
        {
            ++message;
        }
    }
    return purged;
}

void RetainedMessages::forEachMatching(const string_view filter, const Handler& handler) const
{
    // Copied under the lock rather than called under it: the handler publishes, and publishing
    // must not run with the retained store held.
    vector<pair<string, Record>> matched;
    {
        const shared_lock lock(m_mutex);
        for (const auto& [topicName, record]: m_messages)
        {
            if (!record.isTombstone() && matches(filter, topicName))
            {
                matched.emplace_back(topicName, record);
            }
        }
    }

    for (const auto& [topicName, record]: matched)
    {
        handler(topicName, record);
    }
}

void RetainedMessages::forEachRecord(const Handler& handler) const
{
    vector<pair<string, Record>> records;
    {
        const shared_lock lock(m_mutex);
        records.assign(m_messages.begin(), m_messages.end());
    }

    for (const auto& [topicName, record]: records)
    {
        handler(topicName, record);
    }
}

size_t RetainedMessages::size() const
{
    const shared_lock lock(m_mutex);
    return static_cast<size_t>(count_if(m_messages.begin(), m_messages.end(),
                                        [](const auto& message)
                                        {
                                            return !message.second.isTombstone();
                                        }));
}

namespace {

/// The first byte of a record in the format that carries a time. Records written before it begin
/// with their QoS, which is never above 2.
constexpr char TimedRecordFormat = '\x80';

void appendInteger(Buffer& buffer, uint64_t value, const size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
    {
        buffer.append(static_cast<char>(value & 0xFF));
        value >>= 8;
    }
}

bool readInteger(string_view& data, uint64_t& value, const size_t bytes)
{
    if (data.size() < bytes)
    {
        return false;
    }
    value = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (8 * i);
    }
    data.remove_prefix(bytes);
    return true;
}

} // namespace

void RetainedMessages::encode(Buffer& message, const string_view topicName, const Record& record)
{
    appendInteger(message, topicName.size(), sizeof(uint32_t));
    message.append(topicName.data(), topicName.size());
    appendInteger(message, static_cast<uint64_t>(record.m_updated), sizeof(int64_t));
    message.append(static_cast<char>(record.m_qos));
    appendInteger(message, record.m_payload.size(), sizeof(uint32_t));
    message.append(record.m_payload.data(), record.m_payload.size());
}

bool RetainedMessages::decode(string_view message, const Handler& handler)
{
    while (!message.empty())
    {
        uint64_t topicLength = 0;
        if (!readInteger(message, topicLength, sizeof(uint32_t)) || message.size() < topicLength)
        {
            return false;
        }
        const string topicName(message.substr(0, topicLength));
        message.remove_prefix(topicLength);

        Record   record;
        uint64_t updated = 0;
        uint64_t payloadLength = 0;
        if (!readInteger(message, updated, sizeof(int64_t)) || message.empty())
        {
            return false;
        }
        record.m_updated = static_cast<int64_t>(updated);
        record.m_qos = static_cast<Qos>(message[0]);
        message.remove_prefix(1);
        if (!readInteger(message, payloadLength, sizeof(uint32_t)) || message.size() < payloadLength)
        {
            return false;
        }
        record.m_payload.assign(message.substr(0, payloadLength));
        message.remove_prefix(payloadLength);

        if (topicName.empty() || record.m_qos > Qos::Qos2)
        {
            return false;
        }
        handler(topicName, record);
    }
    return true;
}

string RetainedMessages::storageKey(const string& nodeId)
{
    return "node_" + nodeId + "_retained";
}

Buffer RetainedMessages::pack(const Record& record)
{
    // A format byte, the QoS, the time, then the payload - empty in a tombstone. A Buffer rather
    // than a string because a payload is arbitrary bytes, as in the neighbouring session-message
    // records.
    Buffer packed;
    packed.append(TimedRecordFormat);
    packed.append(static_cast<char>(record.m_qos));
    appendInteger(packed, static_cast<uint64_t>(record.m_updated), sizeof(int64_t));
    packed.append(record.m_payload.data(), record.m_payload.size());
    return packed;
}

bool RetainedMessages::unpack(const Buffer& packed, Record& record)
{
    string_view data(bit_cast<const char*>(packed.data()), packed.bytes());
    if (data.empty())
    {
        return false;
    }

    if (data[0] != TimedRecordFormat)
    {
        // Written before records carried a time: the QoS, then a payload that is never empty. It
        // reads as older than any change made since.
        if (data.size() < 2)
        {
            return false;
        }
        record.m_qos = static_cast<Qos>(data[0]);
        record.m_payload.assign(data.substr(1));
        record.m_updated = 0;
        return true;
    }

    data.remove_prefix(1);
    if (data.empty())
    {
        return false;
    }
    record.m_qos = static_cast<Qos>(data[0]);
    data.remove_prefix(1);
    uint64_t updated = 0;
    if (!readInteger(data, updated, sizeof(int64_t)))
    {
        return false;
    }
    record.m_updated = static_cast<int64_t>(updated);
    record.m_payload.assign(data);
    return true;
}

void RetainedMessages::load(const SStorage& storage, const string& nodeId)
{
    if (!storage || !storage->isPersistent())
    {
        return;
    }

    const auto redis = storage->getRedis();
    if (!redis || !redis->isConnected())
    {
        return;
    }

    const auto stored = redis->getHashValues(storageKey(nodeId));

    const unique_lock lock(m_mutex);
    for (const auto& [topicName, packed]: stored)
    {
        if (Record record; unpack(packed.asBuffer(), record))
        {
            m_messages[topicName] = std::move(record);
        }
    }
}

void RetainedMessages::store(const SStorage& storage, const string& nodeId, const string& topicName) const
{
    if (!storage || !storage->isPersistent())
    {
        return;
    }

    const auto redis = storage->getRedis();
    if (!redis || !redis->isConnected())
    {
        return;
    }

    const auto key = storageKey(nodeId);

    Buffer packed;
    {
        const shared_lock lock(m_mutex);
        const auto        message = m_messages.find(topicName);
        if (message == m_messages.end())
        {
            redis->deleteHashKeysAsync(key, {topicName});
            return;
        }
        packed = pack(message->second);
    }

    redis->setHashValueAsync(key, topicName, packed);
}
