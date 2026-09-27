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

#include "common/SessionThread.h"
namespace xmq {

class SessionThreads
{
public:
    explicit SessionThreads(size_t threadCount);
    ~SessionThreads();
    SSessionThread getNextThread();

private:
    mutable std::mutex          m_mutex;
    size_t                      m_nextThreadIndex = 0;
    std::vector<SSessionThread> m_threads;
};

} // namespace xmq
