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

#include "SessionThread.h"

using namespace std;
using namespace sptk;
using namespace xmq;

SessionThread::SessionThread()
{
    initializeSynchronized();
}

void SessionThread::initializeSynchronized()
{
    lock_guard lock(m_mutex);
    m_receiveThread = async(
        [this]
        {
            receiveThreadFunction();
        });
    m_sendThread = async(
        [this]
        {
            sendThreadFunction();
        });
}

SessionThread::~SessionThread()
{
    terminate();

    // Waited for here rather than left to member destruction. Members go in reverse declaration
    // order, so the two queues are destroyed before the futures that join these threads - and a
    // thread still inside pop_front() then unlocks a mutex that no longer exists. terminate()
    // above only asks them to stop; pop_front waits up to 100ms, so there is always a window in
    // which one of them is still in there.
    //
    // libstdc++ lets that pass unnoticed, which is why it survived so long. libc++ traps on it,
    // and every process that had ever opened a session - xmq_pub with nothing but --help among
    // them - died of SIGILL on the way out.
    //
    // Outside the lock terminate() takes: these threads do not need it to finish, but waiting
    // while holding it would be a deadlock the moment they did.
    if (m_sendThread.valid())
    {
        m_sendThread.wait();
    }
    if (m_receiveThread.valid())
    {
        m_receiveThread.wait();
    }
}

void SessionThread::receiveThreadFunction()
{
    Buffer sendBuffer;
    while (!terminated())
    {
        WBaseClientSession wSession;
        if (!m_receiveQueue.pop_front(wSession, 100ms))
        {
            continue;
        }

        if (const auto session = wSession.lock();
            session && session->isConnected())
        {
            session->receiveMessages();
        }
    }
}

void SessionThread::sendThreadFunction()
{
    Buffer sendBuffer;
    while (!terminated())
    {
        if (SendReceiveItem sendReceiveItem;
            m_sendQueue.pop_front(sendReceiveItem, 100ms))
        {
            if (const auto session = sendReceiveItem.m_session.lock())
            {
                try
                {
                    if (session->isConnected())
                    {
                        if (uint32_t remainingExpirationSeconds = 0;
                            sendReceiveItem.m_message->m_message->isExpired(remainingExpirationSeconds))
                        {
                            // Message expired
                            continue;
                        }

                        session->sendMessage(sendReceiveItem.m_message);
                    }
                }
                catch (const Exception& exception)
                {
                    CERR("(" << session->getClientId() << "): " << exception.message());
                }
                catch (const exception& exception)
                {
                    CERR("(" << session->getClientId() << "): " << exception.what());
                }
            }
        }
    }
}

void SessionThread::queueReceiveMessages(WBaseClientSession session)
{
    m_receiveQueue.push_back(std::move(session));
}

void SessionThread::sendMessage(const SBaseClientSession& session, SMessageDispatch& message)
{
    SendReceiveItem sendReceiveItem(session, message);
    m_sendQueue.push_back(std::move(sendReceiveItem));
}

void SessionThread::terminate()
{
    lock_guard lock(m_mutex);
    m_terminated = true;
    m_sendQueue.clear();
    m_receiveQueue.clear();
}

bool SessionThread::terminated() const
{
    return m_terminated;
}

bool SessionThread::sendQueueIsEmpty() const
{
    return m_sendQueue.empty();
}
