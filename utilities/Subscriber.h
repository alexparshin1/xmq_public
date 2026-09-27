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

#include "Utility.h"
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief Test subscriber utility.
 */
class Subscriber final : public Utility
{
public:
    /**
     * @brief Constructor.
     * @param args              Command line arguments.
     */
    explicit Subscriber(const std::vector<std::string>& args);

    /**
     * @brief Run the application (send messages).
     * @return Exit code.
     */
    int run() override;
};

} // namespace xmq
