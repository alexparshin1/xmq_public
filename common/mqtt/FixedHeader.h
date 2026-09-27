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

#include "FrameTypeTests.h"
#include "base/Message.h"

namespace xmq::mqtt {

/**
 * @brief Fixed MQTT packet header.
 */
struct FixedHeader
{
    bool    m_retain : 1;          ///< message isRetain flag
    Qos     m_qos : 2;             ///< message QoS
    bool    m_dup : 1;             ///< message is duplicate
    uint8_t m_type : 4;            ///< message type
    uint8_t m_remainingLength : 8; ///< the first getByte of remaining length

    [[nodiscard]] std::string toString() const
    {
        std::stringstream output;
        output << frameTypeToString(static_cast<FrameTypeTests>(m_type)) << " QOS" << static_cast<int>(m_qos);
        return output.str();
    }
};

} // namespace xmq::mqtt
