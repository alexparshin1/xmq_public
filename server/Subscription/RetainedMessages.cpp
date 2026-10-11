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

RetainedMessages::Change RetainedMessages::set(const string_view topicName, const string_view payload, const Qos qos)
{
    const unique_lock lock(m_mutex);

    const auto message = m_messages.find(string(topicName));
    if (payload.empty())
    {
        if (message == m_messages.end())
        {
            return Change::None;
        }
        m_messages.erase(message);
        return Change::Removed;
    }
    if (message == m_messages.end())
    {
        auto& entry = m_messages[string(topicName)];
        entry.m_record = {string(payload), qos, 0};
        entry.m_present = true;
        return Change::Added;
    }
    auto& record = message->second.m_record;
    if (record.m_payload == payload && record.m_qos == qos)
    {
        return Change::None;
    }
    record.m_payload = payload;
    record.m_qos = qos;
    return Change::Replaced;
}

RetainedMessages::Change RetainedMessages::show(Entry& entry, const Record& committed)
{
    Change change;
    if (committed.clears())
    {
        change = entry.m_present ? Change::Removed : Change::None;
    }
    else if (!entry.m_present)
    {
        change = Change::Added;
    }
    else
    {
        change = entry.m_record.m_payload == committed.m_payload && entry.m_record.m_qos == committed.m_qos
                     ? Change::None
                     : Change::Replaced;
    }
    entry.m_record = committed;
    entry.m_present = !committed.clears();
    return change;
}

RetainedMessages::Change RetainedMessages::takeLocal(const string_view topicName, const string_view payload, const Qos qos)
{
    const unique_lock lock(m_mutex);

    auto& entry = m_messages[string(topicName)];
    if (entry.m_inFlight == 0)
    {
        // The state committed so far, to be weighed against the commits on their way back.
        entry.m_best = entry.m_present ? entry.m_record : Record {"", Qos::Qos0, entry.m_record.m_revision};
    }
    ++entry.m_inFlight;
    return show(entry, {string(payload), qos, 0});
}

RetainedMessages::Change RetainedMessages::applyCommit(const string_view topicName, const Record& committed)
{
    unique_lock lock(m_mutex);

    const auto message = m_messages.find(string(topicName));
    if (message == m_messages.end() || message->second.m_inFlight == 0)
    {
        // Nothing of this node's in flight for it any more - a reconcile cannot take one away, so this
        // is a commit the entry outlived; it weighs as any committed change does.
        lock.unlock();
        bool unknown = false;
        return applyRemote(topicName, committed, true, unknown);
    }

    auto& entry = message->second;
    if (committed.m_revision > entry.m_best.m_revision)
    {
        entry.m_best = committed;
    }
    if (--entry.m_inFlight > 0)
    {
        return Change::None;
    }
    const auto change = show(entry, entry.m_best);
    if (!entry.m_present)
    {
        m_messages.erase(message);
    }
    return change;
}

RetainedMessages::Change RetainedMessages::applyRemote(const string_view topicName, const Record& committed,
                                                       const bool fromStorage, bool& unknown)
{
    unknown = false;
    const unique_lock lock(m_mutex);

    const auto message = m_messages.find(string(topicName));
    if (message == m_messages.end())
    {
        if (committed.clears())
        {
            return Change::None;
        }
        if (!fromStorage)
        {
            unknown = true;
            return Change::None;
        }
        auto& entry = m_messages[string(topicName)];
        return show(entry, committed);
    }

    auto& entry = message->second;
    if (entry.m_inFlight > 0)
    {
        if (committed.m_revision > entry.m_best.m_revision)
        {
            entry.m_best = committed;
        }
        return Change::None;
    }
    if (committed.m_revision <= entry.m_record.m_revision)
    {
        return Change::None;
    }
    const auto change = show(entry, committed);
    if (!entry.m_present)
    {
        m_messages.erase(message);
    }
    return change;
}

void RetainedMessages::reconcile(const size_t shard, const int64_t numbered, const unordered_map<string, Record>& stored,
                                 const Changed& changed)
{
    vector<pair<string, Change>> changes;
    {
        const unique_lock lock(m_mutex);
        for (auto message = m_messages.begin(); message != m_messages.end();)
        {
            auto& [topicName, entry] = *message;
            if (topicName.starts_with('$') || shardOf(topicName) != shard || stored.contains(topicName))
            {
                ++message;
                continue;
            }
            if (entry.m_inFlight > 0 || entry.m_record.m_revision > numbered)
            {
                // A change of this node's on its way to the storage, or one committed after the
                // storage was read: neither was cleared.
                ++message;
                continue;
            }
            changes.emplace_back(topicName, Change::Removed);
            message = m_messages.erase(message);
        }
    }
    for (const auto& [topicName, record]: stored)
    {
        bool       unknown = false;
        const auto change = applyRemote(topicName, record, true, unknown);
        if (change != Change::None)
        {
            changes.emplace_back(topicName, change);
        }
    }
    for (const auto& [topicName, change]: changes)
    {
        changed(topicName, change);
    }
}

size_t RetainedMessages::shardOf(const string_view topicName)
{
    // FNV-1a: std::hash differs between standard libraries, and every node has to agree.
    uint32_t hash = 2166136261U;
    for (const auto character: topicName)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= 16777619U;
    }
    return hash % ShardCount;
}

bool RetainedMessages::parseShared(const string_view stored, Record& record)
{
    const auto revisionEnd = stored.find(':');
    if (revisionEnd == string_view::npos || revisionEnd == 0)
    {
        return false;
    }
    const auto qosEnd = stored.find(':', revisionEnd + 1);
    if (qosEnd != revisionEnd + 2)
    {
        return false;
    }
    int64_t revision = 0;
    for (const auto digit: stored.substr(0, revisionEnd))
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        revision = revision * 10 + (digit - '0');
    }
    const auto qos = stored[revisionEnd + 1] - '0';
    if (qos < 0 || qos > 2)
    {
        return false;
    }
    record.m_revision = revision;
    record.m_qos = static_cast<Qos>(qos);
    record.m_payload.assign(stored.substr(qosEnd + 1));
    return true;
}

void RetainedMessages::forEachMatching(const string_view filter, const Handler& handler) const
{
    // Copied under the lock rather than called under it: the handler publishes, and publishing
    // must not run with the retained store held.
    vector<pair<string, Record>> matched;
    {
        const shared_lock lock(m_mutex);
        for (const auto& [topicName, entry]: m_messages)
        {
            if (entry.m_present && matches(filter, topicName))
            {
                matched.emplace_back(topicName, entry.m_record);
            }
        }
    }

    for (const auto& [topicName, record]: matched)
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
                                            return message.second.m_present;
                                        }));
}

namespace {

/// The first byte of a record in the format that carries a revision - a time, in the versions
/// before. Records written before it begin with their QoS, which is never above 2.
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
    appendInteger(message, static_cast<uint64_t>(record.m_revision), sizeof(int64_t));
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
        uint64_t revision = 0;
        uint64_t payloadLength = 0;
        if (!readInteger(message, revision, sizeof(int64_t)) || message.empty())
        {
            return false;
        }
        record.m_revision = static_cast<int64_t>(revision);
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
    // A format byte, the QoS, the revision, then the payload. A Buffer rather
    // than a string because a payload is arbitrary bytes, as in the neighbouring session-message
    // records.
    Buffer packed;
    packed.append(TimedRecordFormat);
    packed.append(static_cast<char>(record.m_qos));
    appendInteger(packed, static_cast<uint64_t>(record.m_revision), sizeof(int64_t));
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
        // Written before records carried a revision: the QoS, then a payload that is never empty.
        if (data.size() < 2)
        {
            return false;
        }
        record.m_qos = static_cast<Qos>(data[0]);
        record.m_payload.assign(data.substr(1));
        record.m_revision = 0;
        return true;
    }

    data.remove_prefix(1);
    if (data.empty())
    {
        return false;
    }
    record.m_qos = static_cast<Qos>(data[0]);
    data.remove_prefix(1);
    uint64_t revision = 0;
    if (!readInteger(data, revision, sizeof(int64_t)))
    {
        return false;
    }
    record.m_revision = static_cast<int64_t>(revision);
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
        // A clearing left by a version that kept them is nothing to hold.
        if (Record record; unpack(packed.asBuffer(), record) && !record.clears())
        {
            auto& entry = m_messages[topicName];
            entry.m_record = std::move(record);
            entry.m_present = true;
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
        if (message == m_messages.end() || !message->second.m_present)
        {
            redis->deleteHashKeysAsync(key, {topicName});
            return;
        }
        packed = pack(message->second.m_record);
    }

    redis->setHashValueAsync(key, topicName, packed);
}
