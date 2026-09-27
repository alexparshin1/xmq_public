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

#include "BaseClientSession.h"
#include "base/MessageDispatch.h"
#include <future>
#include <sptk5/cthreads>

namespace xmq {

/**
 * @brief Client's session thread.
 */
class SessionThread
{
public:
    struct SendReceiveItem
    {
        WBaseClientSession m_session;
        SMessageDispatch   m_message;
        SendReceiveItem() = default;
        explicit SendReceiveItem(const SBaseClientSession& session)
            : m_session(session)
        {
        }
        SendReceiveItem(const SBaseClientSession& session, SMessageDispatch& message)
            : m_session(session)
            , m_message(std::move(message))
        {
        }
    };

    SessionThread();
    ~SessionThread();
    void queueReceiveMessages(WBaseClientSession session);
    void sendMessage(const SBaseClientSession& session, SMessageDispatch& message);
    void terminate();
    bool sendQueueIsEmpty() const;

protected:
    void receiveThreadFunction();
    void sendThreadFunction();

private:
    mutable std::mutex                          m_mutex;
    // The destructor waits on these before returning; it has to, because the queues below are
    // destroyed first and these threads are still reading them. See ~SessionThread().
    std::future<void>                           m_receiveThread;
    std::future<void>                           m_sendThread;
    std::atomic_bool                            m_terminated {false};
    sptk::SynchronizedQueue<WBaseClientSession> m_receiveQueue;
    sptk::SynchronizedQueue<SendReceiveItem>    m_sendQueue;

    bool terminated() const;
    void initializeSynchronized();
};

using SSessionThread = std::shared_ptr<SessionThread>;

} // namespace xmq
