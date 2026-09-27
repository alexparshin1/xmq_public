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
#include "base/ReasonCode.h"

namespace xmq {

/**
 * @brief Disconnect message.
 */
class XMQ_EXPORT DisconnectMessage final
    : public Message
{
public:
    /**
      * @brief Default constructor.
      */
    DisconnectMessage()
        : Message(Type::Disconnect, 0)
    {
    }

    /**
     * @brief Constructor.
     * @param reasonCode        Disconnect reason code.
     * @param qos               Qos.
     */
    explicit DisconnectMessage(const ReasonCode reasonCode, const Qos qos = Qos::Qos0)
        : Message(Type::Disconnect, 0)
        , m_reasonCode(reasonCode)
    {
        setQos(qos);
    }

    DisconnectMessage(const DisconnectMessage&) = default;

    DisconnectMessage(DisconnectMessage&&) noexcept = default;

    DisconnectMessage& operator=(const DisconnectMessage&) = default;

    DisconnectMessage& operator=(DisconnectMessage&&) noexcept = default;

    /**
         * @brief Destructor.
         */
    ~DisconnectMessage() override = default;

    /**
     * @brief String representation of the message.
     * @return string representation.
     */
    [[nodiscard]] std::string toString() const override
    {
        std::stringstream str;
        str << name() << " id=" << getId()
            << " sender=" << getSender()
            << " qos=" << std::to_string(static_cast<int>(getQos())) << " reason='"
            << xmq::toString(m_reasonCode) << "'.";

        return str.str();
    }

    /**
         * @brief Disconnect reason code.
         */
    [[nodiscard]] ReasonCode getReasonCode() const
    {
        return m_reasonCode;
    }

private:
    ReasonCode m_reasonCode {ReasonCode::Success};
};

} // namespace xmq
