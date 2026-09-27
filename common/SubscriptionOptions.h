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
 * @brief Retain handling.
 */
enum class SubscribeRetainHandling : uint8_t
{
    RetainAlways = 0, ///< Always deliver the retained message
    RetainIfNew = 1,  ///< Deliver the retained message only if the new subscription
    DoNotRetain = 2   ///< Do not deliver the retained message
};

std::string toString(SubscribeRetainHandling retainHandling);

/**
 * @brief Subscription options for destination.
 * @remarks Matches MQTT spec for SUBSCRIBE message.
 */
struct SubscriptionOptions
{
    uint8_t m_maxQos : 2 {2};                ///< maximum QoS
    bool    m_noLocal : 1 {false};           ///< no local subscriptions
    bool    m_retainAsPublished : 1 {false}; ///< isRetain as published
    uint8_t m_retainHandling : 2 {0};        ///< isRetain handling
    uint8_t m_reserved : 2 {0};              ///< reserved

    /**
     * @brief Constructor.
     * @param maxQos            Maximum QoS.
     * @param retainHandling    Retain handling.
     * @param retainAsPublished Retain as published.
     */
    explicit SubscriptionOptions(Qos                     maxQos = Qos::Qos2,
                                 SubscribeRetainHandling retainHandling = SubscribeRetainHandling::RetainAlways,
                                 const bool              retainAsPublished = false)
        : m_maxQos(static_cast<uint8_t>(maxQos))
        , m_retainAsPublished(retainAsPublished)
        , m_retainHandling(static_cast<uint8_t>(retainHandling))
    {
    }

    /**
     * @brief Constructor.
     * @param byte              Subscription options as a byte.
     */
    explicit SubscriptionOptions(const uint8_t byte)
    {
        *std::bit_cast<uint8_t*>(this) = byte;
    }

    void setByte(const uint8_t byte)
    {
        *std::bit_cast<uint8_t*>(this) = byte;
    }

    uint8_t& getByte()
    {
        return *std::bit_cast<uint8_t*>(this);
    }

    [[nodiscard]] Qos getQos() const
    {
        return static_cast<Qos>(m_maxQos);
    }

    void setQos(Qos qos)
    {
        m_maxQos = static_cast<uint8_t>(qos);
    }

    [[nodiscard]] bool getNoLocal() const
    {
        return m_noLocal;
    }

    void setNoLocal(const bool noLocal)
    {
        m_noLocal = noLocal;
    }

    [[nodiscard]] bool getRetainAsPublished() const
    {
        return m_retainAsPublished;
    }

    void setRetainAsPublished(const bool retainAsPublished)
    {
        m_retainAsPublished = retainAsPublished;
    }

    [[nodiscard]] SubscribeRetainHandling getRetainHandling() const
    {
        return static_cast<SubscribeRetainHandling>(m_retainHandling);
    }

    void setRetainHandling(SubscribeRetainHandling retainHandling)
    {
        m_retainHandling = static_cast<uint8_t>(retainHandling);
    }
};

} // namespace xmq
