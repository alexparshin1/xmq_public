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

/**
 * @brief Subscribe Ack message.
 */
class XMQ_EXPORT SubscribeAckMessage final : public AckMessage
{
public:
    /**
     * @brief Constructor.
     * @param messageId         ACKed Subscribe message id.
     * @param subscriptionResults The vector of granted QoS (one per subscription requested by Subscribe message).
     */
    SubscribeAckMessage(const MessageId& messageId, const std::vector<uint8_t>& subscriptionResults)
        : AckMessage(Type::SubscribeAck, messageId)
        , m_subscriptionResults(subscriptionResults)
    {
    }

    /**
     * @brief Constructor.
     * @param messageId         ACKed Subscribe message id.
     * @param subscriptionResults The vector of granted QoS (one per subscription requested by Subscribe message).
     */
    explicit SubscribeAckMessage(const MessageId& messageId)
        : AckMessage(Type::SubscribeAck, messageId)
    {
    }

    /**
     * @brief Destructor.
     */
    ~SubscribeAckMessage() override = default;

    /**
     * @brief Get granted QoS vector (one per subscription requested by Subscribe message).
     * @return granted QoS vector.
     */
    [[nodiscard]] std::vector<uint8_t>& subscriptionResults()
    {
        return m_subscriptionResults;
    }

    /**
     * @brief Get granted QoS vector (one per subscription requested by Subscribe message).
     * @return granted QoS vector.
     */
    [[nodiscard]] const std::vector<uint8_t>& subscriptionResults() const
    {
        return m_subscriptionResults;
    }

    /**
     * @brief Get message string representation.
     * @return message string representation.
     */
    [[nodiscard]] std::string toString() const override
    {
        std::stringstream str;
        str << name() << " id=" << static_cast<int>(getId())
            << " grantedQOS=[";

        sptk::Strings subscriptionResults;
        for (const auto& subscriptionResult: m_subscriptionResults)
        {
            subscriptionResults.push_back(std::to_string(subscriptionResult));
        }

        str << subscriptionResults.join(",") << "]";

        return str.str();
    }

private:
    std::vector<uint8_t> m_subscriptionResults; ///< Subscription result vector (one per topic requested by Subscribe message)
};

using SSubscribeAckMessage = std::shared_ptr<SubscribeAckMessage>;

} // namespace xmq
