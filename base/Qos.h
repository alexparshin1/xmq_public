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

#include <cstdint>
#include <string>

namespace xmq {

/**
 * @brief QoS.
 */
enum class Qos : uint8_t
{
    Qos0 = 0,
    Qos1 = 1,
    Qos2 = 2,
    Invalid = 3,
    // Compatibility aliases
    AtMostOnce = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2
};

} // namespace xmq

std::string to_string(xmq::Qos qos);
