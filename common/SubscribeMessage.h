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

#include "Destination.h"
#include "base/Message.h"

#include <utility>

namespace xmq {

/**
 * @brief Subscribe message.
 */
class XMQ_EXPORT SubscribeMessage final : public Message
{
public:
    /**
     * @brief Default constructor.
     */
    explicit SubscribeMessage();

    /**
     * @brief Constructor.
     * @param destinations      message destination.
     */
    explicit SubscribeMessage(Destinations destinations);

    SubscribeMessage(const SubscribeMessage&) = delete;
    SubscribeMessage(SubscribeMessage&&) noexcept = default;
    SubscribeMessage& operator=(const SubscribeMessage&) = delete;
    SubscribeMessage& operator=(SubscribeMessage&&) noexcept = default;

    /**
     * @brief Destructor.
     */
    ~SubscribeMessage() override = default;

    /**
     * @brief Get message destinations (AKA subscriptions).
     * @return message destinations.
     */
    void setDestinations(Destinations&& destinations)
    {
        m_destinations = std::move(destinations);
    }

    /**
     * @brief Get message destinations (AKA subscriptions).
     * @return message destinations.
     */
    [[nodiscard]] Destinations& getDestinations()
    {
        return m_destinations;
    }

    /**
     * @brief Get message destinations (AKA subscriptions).
     * @return message destinations.
     */
    [[nodiscard]] const Destinations& getDestinations() const
    {
        return m_destinations;
    }

    /**
     * @brief Get message string representation.
     * @return message string representation.
     */
    [[nodiscard]] std::string toString() const override;

private:
    Destinations m_destinations;       ///< message destinations
    uint64_t     m_subscriptionId {0}; ///< Subscription ID
};

} // namespace xmq
