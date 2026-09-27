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

    if (payload.empty())
    {
        return m_messages.erase(string(topicName)) != 0 ? Change::Removed : Change::None;
    }

    const auto [message, inserted] = m_messages.try_emplace(string(topicName));
    auto&      record = message->second;

    if (!inserted && record.m_payload == payload && record.m_qos == qos)
    {
        return Change::None;
    }

    record.m_payload = payload;
    record.m_qos = qos;
    return inserted ? Change::Added : Change::Replaced;
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
            if (matches(filter, topicName))
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

size_t RetainedMessages::size() const
{
    const shared_lock lock(m_mutex);
    return m_messages.size();
}

string RetainedMessages::storageKey(const string& nodeId)
{
    return "node_" + nodeId + "_retained";
}

Buffer RetainedMessages::pack(const Record& record)
{
    // One byte of QoS then the payload. A Buffer rather than a string because a payload is
    // arbitrary bytes - the neighbouring session-message records are written the same way.
    Buffer packed;
    packed.append(static_cast<char>(record.m_qos));
    packed.append(record.m_payload.data(), record.m_payload.size());
    return packed;
}

bool RetainedMessages::unpack(const Buffer& packed, Record& record)
{
    if (packed.bytes() < 2)
    {
        return false;
    }
    const auto* data = bit_cast<const char*>(packed.data());
    record.m_qos = static_cast<Qos>(data[0]);
    record.m_payload.assign(data + 1, packed.bytes() - 1);
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
