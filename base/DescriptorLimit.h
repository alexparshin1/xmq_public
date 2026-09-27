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

#include <algorithm>
#include <cstddef>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace xmq {

/**
 * @brief The most connections this process could ever hold at once.
 *
 * Every connection costs a descriptor, so the descriptor limit is a ceiling the operating system
 * already knows - nothing for anyone to configure, and nothing for anyone to get wrong. It is used
 * to size the maps that hold one entry per connection, because growing such a map rehashes
 * everything it holds under a lock the whole broker needs.
 *
 * Capped, because the limit may be RLIM_INFINITY, and because reserving beyond what any host will
 * really hold buys nothing.
 * @param cap Largest answer to give.
 */
inline size_t possibleConnectionCount(const size_t cap = 4000000)
{
#ifndef _WIN32
    if (rlimit descriptorLimit {}; getrlimit(RLIMIT_NOFILE, &descriptorLimit) == 0 &&
                                   descriptorLimit.rlim_cur != RLIM_INFINITY)
    {
        return std::min(static_cast<size_t>(descriptorLimit.rlim_cur), cap);
    }
#endif
    return cap;
}

} // namespace xmq
