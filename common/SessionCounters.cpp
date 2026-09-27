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

#include "SessionCounters.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void SessionCounters::resetCounters()
{
    m_publishReceiveCount = 0;
    m_publishScheduledCount = 0;
    m_publishSendCount = 0;
}
size_t SessionCounters::incrementPublishReceiveCount()
{
    ++m_publishReceiveCount;
    return m_publishReceiveCount;
}

size_t SessionCounters::incrementPublishScheduledCount()
{
    ++m_publishScheduledCount;
    return m_publishScheduledCount;
}

size_t SessionCounters::incrementPublishSendCount()
{
    ++m_publishSendCount;
    return m_publishSendCount;
}

size_t SessionCounters::getPublishReceiveCount() const
{
    return m_publishReceiveCount;
}

size_t SessionCounters::getPublishScheduledCount() const
{
    return m_publishScheduledCount;
}

size_t SessionCounters::getPublishSendCount() const
{
    return m_publishSendCount;
}
