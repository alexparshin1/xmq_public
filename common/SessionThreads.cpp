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

#include "SessionThreads.h"

using namespace std;
using namespace xmq;

SessionThreads::SessionThreads(const size_t threadCount)
{
    for (size_t i = 0; i < threadCount; ++i)
    {
        lock_guard lock(m_mutex);
        auto       thread = make_shared<SessionThread>();
        m_threads.push_back(thread);
    }
}

SSessionThread SessionThreads::getNextThread()
{
    lock_guard lock(m_mutex);
    ++m_nextThreadIndex;
    if (m_nextThreadIndex >= m_threads.size())
    {
        m_nextThreadIndex = 0;
    }
    return m_threads[m_nextThreadIndex];
}

SessionThreads::~SessionThreads()
{
    lock_guard lock(m_mutex);
    for (auto& thread: m_threads)
    {
        thread->terminate();
        thread.reset();
    }
    m_threads.clear();
}
