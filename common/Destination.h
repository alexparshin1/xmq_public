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

#include "SubscriptionOptions.h"
#include "base/Message.h"
#include "base/Topic.h"
#include <sptk5/cnet>

namespace xmq {

/**
 * @brief Message destination.
 */
struct Destination
{
    const Topic*        m_topic {nullptr};  ///< Topic
    SubscriptionOptions m_subscribeOptions; ///< Subscribe options. MQTT3x only uses Qos.

    /**
     * @brief Constructor.
     * @param topic             Topic.
     * @param options           Subscribe options.
     */
    explicit Destination(const Topic* topic, const SubscriptionOptions& options = SubscriptionOptions(Qos::Qos1))
        : m_topic(topic)
        , m_subscribeOptions(options)
    {
    }

    /**
     * @brief String representation of the destination
     * @return String representation
     */
    [[nodiscard]] sptk::String toString() const
    {
        std::stringstream str;
        str << m_topic->toString()
            << ":qos" << static_cast<int>(m_subscribeOptions.m_maxQos)
            << ":" << xmq::toString(static_cast<SubscribeRetainHandling>(m_subscribeOptions.m_retainHandling));
        if (m_subscribeOptions.m_retainAsPublished)
        {
            str << ":RetainAsPublished";
        }
        // No Local decides whether a subscriber is sent back what it published itself, which is
        // how a bridge keeps traffic from going round in circles. Leaving it out of the log made
        // a subscription that carried it look identical to one that did not.
        if (m_subscribeOptions.m_noLocal)
        {
            str << ":NoLocal";
        }
        return str.str();
    }
};

/**
 * @brief Vector of destinations type
 */
using Destinations = std::vector<Destination>;

} // namespace xmq
