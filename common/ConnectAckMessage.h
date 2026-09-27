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

#include "base/AckMessage.h"

namespace xmq {

class XMQ_EXPORT ConnectAckMessage final : public AckMessage
{
public:
    /**
     * @brief Constructor.
     */
    ConnectAckMessage(const ReasonCode reasonCode, const bool sessionPresent)
        : AckMessage(Type::ConnectAck, 0)
        , m_sessionPresent(sessionPresent)
    {
        setReasonCode(reasonCode);
    }

    /**
     * @brief Destructor.
     */
    ~ConnectAckMessage() override = default;

    bool isSessionPresent() const
    {
        return m_sessionPresent;
    }

private:
    bool m_sessionPresent;
};

} // namespace xmq
