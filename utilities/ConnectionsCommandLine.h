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

#include "UtilityCommandLine.h"

namespace xmq {

class ConnectionsCommandLine final : public UtilityCommandLine
{
public:
    /**
     * @brief Constructor.
     */
    explicit ConnectionsCommandLine(const std::vector<std::string>& args);

    /**
     * @brief Destructor.
     */
    ~ConnectionsCommandLine() override = default;
};

} // namespace xmq
