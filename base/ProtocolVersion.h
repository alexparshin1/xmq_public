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

/**
 * XMQ Protocol versions
 */
enum class ProtocolVersion : uint8_t
{
    MqttV31 = 3,  ///< MQTT v3.1
    MqttV311 = 4, ///< MQTT v3.1.1
    MqttV5 = 5    ///< MQTT v5
};

} // namespace xmq

/**
 * @brief Convert protocol version to string.
 * @param protocolVersion Protocol version.
 * @return protocol version string.
 */
std::string to_string(xmq::ProtocolVersion protocolVersion);
