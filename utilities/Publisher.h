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
#include "client/MqttClient.h"
#include <sptk5/cutils>

namespace xmq {

class Publisher final : public Utility
{
public:
    explicit Publisher(const std::vector<std::string>& args);

    /**
     * @brief Run the application (send messages).
     * @return Exit code.
     */
    int run() override;
    int sendMessages(sptk::Semaphore& allMessagesDelivered) const;
};

} // namespace xmq
