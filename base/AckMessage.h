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

#include "base/Message.h"

#include <memory>

namespace xmq {

/**
 * @brief Generic ACK message.
 */
class XMQ_EXPORT AckMessage : public Message
{
public:
    using Message::Message;

    /**
     * @brief Destructor.
     */
    ~AckMessage() override = default;

    /**
     * @brief Returns string representation.
     * @return String representation.
     */
    [[nodiscard]] std::string toString() const override;

    [[nodiscard]] ReasonCode getReasonCode() const
    {
        return m_reasonCode;
    }

    void setReasonCode(const ReasonCode reasonCode)
    {
        m_reasonCode = reasonCode;
    }

private:
    ReasonCode m_reasonCode {ReasonCode::Success};
};

using SAckMessage = std::shared_ptr<AckMessage>;

} // namespace xmq
