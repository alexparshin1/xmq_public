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

#include "StaticCounter.h"

using namespace std;
using namespace sptk;
using namespace xmq;

StaticCounter& StaticCounter::operator++()
{
    lock_guard lock(m_mutex);

    ++m_totalMessages;

    if (m_totalMessages == 1)
    {
        m_lastTime = chrono::steady_clock::now();
    }
    else if (m_totalMessages % m_batchSize == 0)
    {
        const auto batchDuration = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - m_lastTime);
        COUT(setw(14) << right << m_totalMessages << setw(14) << right << batchDuration.count() << "ms");
        m_lastTime = chrono::steady_clock::now();
    }

    return *this;
}
