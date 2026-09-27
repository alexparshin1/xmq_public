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

#include <base/xmq.h>
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief Session thread serial number manager.
 * @remarks If a sessionId wasn't seen before, it will be assigned a new serial number. Otherwise, the existing serial number is returned. Thread-safe.
 */
class XMQ_EXPORT ClientSessionSerial
{
public:
    /**
     * @brief Get a serial number for a session.
     * @param sessionId Session identifier.
     * @return Serial number for the session.
     */
    uint64_t getSerial(const std::string& sessionId);

private:
    mutable std::mutex                        m_mutex;      ///< Mutex for thread-safe access.
    std::unordered_map<std::string, uint64_t> m_serials;    ///< Session serial number map.
    uint64_t                                  m_serial {0}; ///< Serial number counter.
};

} // namespace xmq
