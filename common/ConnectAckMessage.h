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
     * @brief CONNACK user property: this broker stores what it accepts durably.
     *
     * A durable broker holds a QoS 1 publication in its journal until the subscriber
     * acknowledges it, so the acknowledgement is a flow-control decision and not just a
     * courtesy: an acknowledgement written sixteen messages later leaves sixteen messages
     * per subscriber held in the journal. A client that reads this property writes each
     * acknowledgement on its own, whatever its own batching default says.
     */
    static constexpr std::string_view PersistenceProperty {"$xmq-persistence"};
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
