/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "Topic.h"

using namespace std;
using namespace sptk;
using namespace xmq;

Topic::Topic(const string_view topicName)
    : m_buffer(topicName)
    , m_shareGroupName("", 0)
{
    initPath();
}

void Topic::analyzeDollarTopic(const char*& elementStart)
{
    if (strncasecmp(elementStart, "$share/", 7) == 0)
    {
        m_isShared = true;
        constexpr auto groupNameOffset {7};
        elementStart += groupNameOffset;
        if (auto* groupEnd = strchr(elementStart, '/'))
        {
            // m_fullName is left empty here: initPath() then sets it to the whole topic name,
            // so it includes the "$share/<group>/" prefix as documented.
            m_shareGroupName = {elementStart, static_cast<size_t>(groupEnd - elementStart)};
            elementStart = groupEnd + 1;
        }
    }
    else if (strncasecmp(elementStart, "$sys/", 5) == 0)
    {
        m_isSystem = true;
    }
    else if (strncasecmp(elementStart, "$cluster/", 9) == 0)
    {
        m_isCluster = true;
    }
    else
    {
        throw Exception("Invalid topic name '" + string(m_buffer.c_str()) + "'");
    }
}

void Topic::initPath()
{
    constexpr auto initialElementNumber {3};
    m_path.reserve(initialElementNumber);

    const char* elementStart = m_buffer.c_str();
    m_isWildcard = false;

    m_isShared = false;
    if (*elementStart == '$')
    {
        analyzeDollarTopic(elementStart);
    }

    if (m_fullName.empty())
    {
        m_fullName = {m_buffer.c_str(), m_buffer.size()};
    }

    const char* scanStart = elementStart;

    do
    {
        scanStart = strpbrk(scanStart, "#+");
        if (scanStart != nullptr)
        {
            m_isWildcard = true;
            if (*scanStart == '#' && *(scanStart + 1) == '/')
            {
                m_buffer.bytes(scanStart - elementStart + 1);
                elementStart = m_buffer.c_str();
                break;
            }
            ++scanStart;
        }
    } while (scanStart != nullptr);

    m_path.clear();
    while (true)
    {
        auto* elementEnd = strchr(elementStart, '/');
        if (elementEnd != nullptr)
        {
            m_path.emplace_back(elementStart, elementEnd - elementStart);
        }
        else
        {
            m_path.emplace_back(elementStart, m_buffer.c_str() + m_buffer.size() - elementStart);
            break;
        }
        elementStart = elementEnd + 1;
    }

    if (!m_path[0].empty() && m_path[0][0] == '/')
    {
        throw Exception("Invalid topic name '" + string(m_fullName) + "'");
    }

    for (const auto& element: m_path)
    {
        if ((element.empty() && m_path.size() > 1) ||
            (element.length() != 1 && element.find_first_of("#+") != string::npos))
        {
            throw Exception("Invalid topic name '" + string(m_fullName) + "'");
        }
    }
}

const Topic* TopicManager::getTopic(const string_view topicName)
{
    const ReadWriteLock lock(m_factoryMutex, ReadWriteLock::Mode::Reader);

    static const auto emptyTopic = shared_ptr<Topic>(new Topic(""));

    auto iterator = m_cachedTopics.find(topicName);
    if (iterator == m_cachedTopics.end())
    {
        if (topicName.empty())
        {
            return emptyTopic.get();
        }

        lock.upgradeToWriteLock();

        STopic topic(new Topic(topicName));

        auto [it, _] = m_cachedTopics.try_emplace(topic->name(), topic);
        iterator = it;
    }
    return iterator->second.get();
}

const Topic* TopicManager::getTopic(const Topic& topic)
{
    return getTopic(topic.name());
}

size_t TopicManager::size()
{
    const ReadWriteLock lock(m_factoryMutex, ReadWriteLock::Mode::Reader);
    return m_cachedTopics.size();
}

std::shared_ptr<Topic> TopicManager::get(const std::string_view topicName)
{
    const ReadWriteLock lock(m_factoryMutex, ReadWriteLock::Mode::Reader);

    if (const auto it = m_cachedTopics.find(topicName);
        it != m_cachedTopics.end())
    {
        return it->second;
    }

    return {};
}

const Topic* TopicManager::emptyTopic()
{
    static const Topic emptyTopic("");
    return &emptyTopic;
}
