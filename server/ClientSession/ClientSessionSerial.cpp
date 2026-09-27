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

#include "ClientSessionSerial.h"

using namespace std;
using namespace sptk;

namespace xmq {

uint64_t ClientSessionSerial::getSerial(const std::string& sessionId)
{
    if (sessionId.empty())
    {
        return 0;
    }

    const scoped_lock lock(m_mutex);

    auto it = m_serials.find(sessionId);
    if (it == m_serials.end())
    {
        ++m_serial;
        it = m_serials.emplace(sessionId, m_serial).first;
    }

    return it->second;
}

} // namespace xmq
