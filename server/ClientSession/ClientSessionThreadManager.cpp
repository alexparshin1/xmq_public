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

#include "ClientSessionThreadManager.h"
#include "ClientSessionSendThread.h"
#include "ClientSessionThread.h"

using namespace std;
using namespace xmq;
using namespace sptk;

ClientSessionThreadManager::ClientSessionThreadManager(Server* server, const size_t sendThreadCount, const size_t receiveThreadCount, LogEngine& logEngine)
{
    for (size_t i = 0; i < receiveThreadCount; ++i)
    {
        auto thread = make_shared<ClientSessionReceiveThread>(server, logEngine);
        m_receiveThreads.push_back(thread);
    }

    for (size_t i = 0; i < sendThreadCount; ++i)
    {
        auto thread = make_shared<ClientSessionSendThread>(server, logEngine);
        m_sendThreads.push_back(thread);
    }
}

ClientSessionThreadManager::~ClientSessionThreadManager()
{
    stop();

    m_receiveThreads.clear();
    m_sendThreads.clear();
}

void ClientSessionThreadManager::stop()
{
    for (const auto& thread: m_receiveThreads)
    {
        thread->terminate();
    }

    for (const auto& thread: m_sendThreads)
    {
        thread->terminate();
    }
}

ClientSessionThreads ClientSessionThreadManager::getClientSessionThreads()
{
    ClientSessionThreads sessionThreads;

    std::scoped_lock lock(m_mutex);

    m_nextSendThreadIndex = (m_nextSendThreadIndex + 1) % m_sendThreads.size();
    sessionThreads.m_sendThread = m_sendThreads[m_nextSendThreadIndex];

    m_nextReceiveThreadIndex = (m_nextReceiveThreadIndex + 1) % m_receiveThreads.size();
    sessionThreads.m_receiveThread = m_receiveThreads[m_nextReceiveThreadIndex];

    return sessionThreads;
}

size_t ClientSessionThreadManager::receiveQueueLength() const
{
    std::scoped_lock lock(m_mutex);
    size_t           total = 0;
    for (const auto& thread: m_receiveThreads)
    {
        total += thread->sessionQueueLength();
    }
    return total;
}

size_t ClientSessionThreadManager::sendQueueLength() const
{
    std::scoped_lock lock(m_mutex);
    size_t           total = 0;
    for (const auto& thread: m_sendThreads)
    {
        total += thread->sessionQueueLength();
    }
    return total;
}
