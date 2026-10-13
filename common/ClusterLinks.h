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

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xmq {

/**
 * @brief Which of the links between two nodes carries a topic.
 *
 * Both ends have to agree, and the nodes may be built with different standard libraries, so this
 * is FNV-1a rather than std::hash.
 *
 * @param topicName         Topic name, or an exact topic filter.
 * @param linkCount         How many links there are between the two nodes.
 * @return the index of the link.
 */
inline size_t clusterLinkOf(const std::string_view topicName, const size_t linkCount)
{
    if (linkCount <= 1)
    {
        return 0;
    }
    uint32_t hash = 2166136261U;
    for (const auto character: topicName)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= 16777619U;
    }
    return hash % linkCount;
}

} // namespace xmq
