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

class StaticCounter final
{
public:
    /**
     * @brief Constructor
     */
    explicit StaticCounter(const size_t batchSize = 10000)
        : m_batchSize(batchSize)
    {
    }

    /**
     * @brief Destructor
     */
    ~StaticCounter() = default;

    void reset(const size_t batchSize = 0)
    {
        std::lock_guard lock(m_mutex);
        m_totalMessages = 0;
        m_lastTime = std::chrono::steady_clock::now();
        if (batchSize)
        {
            m_batchSize = batchSize;
        }
    }

    StaticCounter& operator++();

private:
    std::mutex                            m_mutex;
    size_t                                m_batchSize {10000};
    size_t                                m_totalMessages {0};
    std::chrono::steady_clock::time_point m_lastTime;
};

} // namespace xmq
