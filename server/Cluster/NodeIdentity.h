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

#include "base/xmq.h"

#include <filesystem>
#include <string>

namespace xmq::cluster {

/**
 * @brief A node's GUID: who the node is to the cluster, whatever it is called.
 *
 * Made once, on the node's first start, and kept in a file of its own beside the configuration
 * (xmq_node.id) - not in the configuration, which is what gets copied to set up the next node, and
 * would take the GUID with it. The name is for people and may be changed; the GUID is not.
 */
class XMQ_EXPORT NodeIdentity
{
public:
    static constexpr const char* FileName = "xmq_node.id";

    /**
     * @brief The node's GUID, made and saved if the file does not hold one yet.
     * @param file              Where the GUID is kept.
     * @return The GUID.
     * @throws sptk::Exception  The file can't be written.
     */
    static std::string load(const std::filesystem::path& file);

    /**
     * @return A new random GUID (version 4).
     */
    static std::string generate();

    /**
     * @param text              Text to check.
     * @return True if the text is a GUID as generate() writes it.
     */
    static bool isGuid(const std::string& text);
};

} // namespace xmq::cluster
