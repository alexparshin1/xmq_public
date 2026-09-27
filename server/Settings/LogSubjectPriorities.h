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

#include "LogSubject.h"


#include <service/CLogging.h>
#include <shared_mutex>
#include <sptk5/LogPriority.h>

namespace xmq {

constexpr size_t LogSubjectCount = 11;

class LogSubjectPriorities
{
public:
    /**
     * @brief Constructor
     */
    explicit LogSubjectPriorities();
    /**
     * @brief Destructor
     */
    virtual ~LogSubjectPriorities() = default;

    void initialize(const CLogging& logging);

    sptk::LogPriority getLogPriority() const;
    void              setLogPriority(sptk::LogPriority logPriority);
    void              setLogSubjectPriority(LogSubject messageSubject, sptk::LogPriority messagePriority);
    void              setLogSubjectsPriority(std::initializer_list<LogSubject> messageSubjects, sptk::LogPriority priority);
    void              setLogSubjectsPriority(const std::vector<LogSubject>& messageSubjects, sptk::LogPriority messagePriority);
    bool              logSubjectEnabled(LogSubject messageSubject, sptk::LogPriority messagePriority) const;

private:
    mutable std::shared_mutex                      m_mutex;
    CLogging                                       m_logging;                                ///< Logging settings
    sptk::LogPriority                              m_logPriority {sptk::LogPriority::Debug}; ///< Default log priority
    std::array<sptk::LogPriority, LogSubjectCount> m_logSubjectPriorities {};                ///< Log subjects priorities
};

} // namespace xmq
