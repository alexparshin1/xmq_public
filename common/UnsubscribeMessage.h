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

namespace xmq {

/**
 * @brief Unsubscribe message.
 */
class XMQ_EXPORT UnsubscribeMessage final : public Message
{
public:
    /**
     * @brief Default constructor.
     */
    UnsubscribeMessage()
        : Message(Type::Unsubscribe)
    {
        setQos(Qos::Qos1);
    }

    UnsubscribeMessage(const UnsubscribeMessage&) = delete;
    UnsubscribeMessage(UnsubscribeMessage&&) = delete;
    UnsubscribeMessage& operator=(const UnsubscribeMessage&) = delete;
    UnsubscribeMessage& operator=(UnsubscribeMessage&&) = delete;

    /**
     * @brief Constructor.
     * @param destinations      Message destinations (subscriptions) to unsubscribe from.
     */
    explicit UnsubscribeMessage(Destinations destinations)
        : Message(Type::Unsubscribe)
        , m_destinations(std::move(destinations))
    {
        setQos(Qos::Qos1);
    }

    /**
     * @brief Destructor.
     */
    ~UnsubscribeMessage() override = default;

    void setDestinations(Destinations&& destinations)
    {
        m_destinations = std::move(destinations);
    }

    /**
     * @brief Get message destinations to unsubscribe from.
     * @return message destinations.
     */
    [[nodiscard]] const Destinations& destinations() const
    {
        return m_destinations;
    }

    /**
     * @brief Get message string representation.
     * @return message string representation.
     */
    [[nodiscard]] std::string toString() const override
    {
        std::stringstream str;
        str << name() << " id=" << static_cast<int>(getId())
            << " destinations=[";

        sptk::Strings destinationStrings;
        for (const auto& destination: m_destinations)
        {
            destinationStrings.push_back(destination.toString());
        }
        str << destinationStrings.join(", ") << "]";

        return str.str();
    }

private:
    Destinations m_destinations; ///< Message destinations to unsubscribe from
};

} // namespace xmq
