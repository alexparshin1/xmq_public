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

#include "LogSubjectPriorities.h"

#include "LogSubject.h"

using namespace std;
using namespace sptk;
using namespace xmq;

using enum LogSubject;

const std::map<String, LogSubject> availableLogSubjects = {
    {"ack", Ack},
    {"publish", Publish},
    {"subscribe", Subscribe},
    {"unsubscribe", Unsubscribe},
    {"connect", Connect},
    {"disconnect", Disconnect},
    {"server_connections", ServerConnections},
    {"server_events", ServerEvents},
    {"cluster_connections", ClusterConnections},
    {"cluster_events", ClusterEvents},
};

LogSubjectPriorities::LogSubjectPriorities()
{
    unique_lock lock(m_mutex);
    for (auto& logPriority: m_logSubjectPriorities)
    {
        logPriority = LogPriority::Info;
    }
}

void LogSubjectPriorities::initialize(const CLogging& logging)
{
    unique_lock lock(m_mutex);

    m_logging = logging;

    const auto minLogPriorityStr = logging.m_min_log_level.asString();
    m_logPriority = LogEngine::priorityFromName(minLogPriorityStr.empty() ? "Info" : minLogPriorityStr);
    for (const auto& [subjectString, subject]: availableLogSubjects)
    {
        const auto subjectLevel = logging.m_log_level.getField(subjectString)->asString();
        const auto priority = LogEngine::priorityFromName(subjectLevel);
        const auto subjectIndex = static_cast<size_t>(subject);
        const auto clippedPriority = priority >= m_logPriority ? m_logPriority : priority;
        m_logSubjectPriorities[subjectIndex] = clippedPriority;
    }
    m_logSubjectPriorities[static_cast<size_t>(SessionErrors)] = LogPriority::Error;
}

LogPriority LogSubjectPriorities::getLogPriority() const
{
    shared_lock lock(m_mutex);
    return m_logPriority;
}

void LogSubjectPriorities::setLogPriority(const LogPriority logPriority)
{
    unique_lock lock(m_mutex);
    m_logPriority = logPriority;
    for (const auto& [subjectString, subject]: availableLogSubjects)
    {
        const auto subjectLevel = m_logging.m_log_level.getField(subjectString)->asString();
        const auto priority = LogEngine::priorityFromName(subjectLevel);
        const auto subjectIndex = static_cast<size_t>(subject);
        const auto clippedPriority = priority >= m_logPriority ? m_logPriority : priority;
        m_logSubjectPriorities[subjectIndex] = clippedPriority;
    }
    m_logSubjectPriorities[static_cast<size_t>(SessionErrors)] = LogPriority::Error;
}

void LogSubjectPriorities::setLogSubjectPriority(LogSubject messageSubject, const LogPriority messagePriority)
{
    unique_lock lock(m_mutex);
    if (const auto subjectIndex = static_cast<size_t>(messageSubject);
        subjectIndex < LogSubjectCount)
    {
        m_logSubjectPriorities[subjectIndex] = messagePriority;
    }
}

void LogSubjectPriorities::setLogSubjectsPriority(const std::initializer_list<LogSubject> messageSubjects, const LogPriority priority)
{
    if (messageSubjects.size() == 0)
    {
        unique_lock lock(m_mutex);
        for (auto& subjectPriority: m_logSubjectPriorities)
        {
            subjectPriority = priority;
        }
        return;
    }

    for (const auto subject: messageSubjects)
    {
        setLogSubjectPriority(subject, priority);
    }
}

void LogSubjectPriorities::setLogSubjectsPriority(const vector<LogSubject>& messageSubjects, const LogPriority messagePriority)
{
    for (const auto subject: messageSubjects)
    {
        setLogSubjectPriority(subject, messagePriority);
    }
}

bool LogSubjectPriorities::logSubjectEnabled(LogSubject messageSubject, const LogPriority messagePriority) const
{
    shared_lock lock(m_mutex);

    if (const auto subjectIndex = static_cast<size_t>(messageSubject);
        subjectIndex < LogSubjectCount)
    {
        const auto priority = m_logSubjectPriorities[subjectIndex];
        return priority >= messagePriority;
    }
    return false;
}
