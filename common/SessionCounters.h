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

#include <sptk5/cutils>

namespace xmq {

class SessionCounters
{
public:
    SessionCounters() = default;
    SessionCounters(const SessionCounters&) = delete;
    SessionCounters(const SessionCounters&&) = delete;
    SessionCounters& operator=(const SessionCounters&) = delete;
    SessionCounters& operator=(const SessionCounters&&) = delete;
    virtual ~SessionCounters() = default;

    void resetCounters();

    size_t incrementPublishReceiveCount();
    size_t incrementPublishScheduledCount();
    size_t incrementPublishSendCount();

    size_t getPublishReceiveCount() const;
    size_t getPublishScheduledCount() const;
    size_t getPublishSendCount() const;

private:
    std::atomic_size_t m_publishReceiveCount {0};   ///< Number of Publish messages received
    std::atomic_size_t m_publishScheduledCount {0}; ///< Number of Publish messages received
    std::atomic_size_t m_publishSendCount {0};      ///< Number of Publish messages sent
};

} // namespace xmq
