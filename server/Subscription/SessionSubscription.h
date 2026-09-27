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

#include "common/ISubscriptionClient.h"
#include "common/SubscriptionOptions.h"
#include "storage/PersistentObject.h"

namespace xmq {

class ClientSession;
class Subscription;

/**
 * @brief Persistent link representing client's subscription with QoS and optional subscriptionId.
 */
class XMQ_EXPORT SessionSubscription final
{
public:
    /**
     * @brief Constructor.
     * @param subscriptionClient Subscription client.
     * @param subscription      Subscription.
     * @param qos               Subscription max QoS.
     * @param subscriptionOptions Subscription options.
     * @param subscriptionId    Optional subscription id.
     */
    static std::shared_ptr<SessionSubscription> create(const std::shared_ptr<ISubscriptionClient>& subscriptionClient,
                                                       std::shared_ptr<Subscription>               subscription,
                                                       Qos                                         qos,
                                                       SubscriptionOptions                         subscriptionOptions,
                                                       uint32_t                                    subscriptionId = 0);

    /**
     * @brief Copy constructor (deleted).
     */
    SessionSubscription(const SessionSubscription&) = delete;

    /**
     * @brief Move constructor (deleted).
     */
    SessionSubscription(SessionSubscription&&) = delete;

    SessionSubscription& operator=(const SessionSubscription&) = delete;
    SessionSubscription& operator=(SessionSubscription&&) = delete;

    /**
     * @brief Destructor.
     */
    ~SessionSubscription() = default;

    /**
     * @brief Set the client session.
     * @param clientConnection  Client session.
     */
    void setClientConnection(const std::shared_ptr<ISubscriptionClient>& clientConnection)
    {
        m_clientSession = clientConnection;
    }

    /**
     * @brief Get the client session.
     */
    [[nodiscard]] std::shared_ptr<ISubscriptionClient> clientSession() const
    {
        return m_clientSession;
    }

    [[nodiscard]] bool retainDelivered() const;

    void setRetainDelivered(bool retainDelivered);

    /**
     * @brief Get this object's connection record id.
     */
    [[nodiscard]] RecordId sessionRecordId() const;

    /**
     * @brief Get this object's subscription QoS.
     * @return.
     */
    [[nodiscard]] Qos qos() const
    {
        return m_qos;
    }

    [[nodiscard]] SubscriptionOptions options() const
    {
        return m_subscriptionOptions;
    }

    /**
     * @brief Get this object's optional subscription id.
     */
    [[nodiscard]] uint32_t subscriptionId() const
    {
        return m_subId;
    }

private:
    uint32_t                             m_subId {0};              ///< Optional subscription id
    Qos                                  m_qos {0};                ///< Subscription max QoS
    SubscriptionOptions                  m_subscriptionOptions {}; ///< Subscription options
    std::shared_ptr<ISubscriptionClient> m_clientSession;          ///< Client session
    std::shared_ptr<Subscription>        m_subscription;           ///< Subscription.
    uint8_t                              m_retainDelivered {0};    ///< Retain delivered flag

    /**
     * @brief Constructor.
     * @param clientSession     Client connection.
     * @param subscription      Subscription.
     * @param qos               Subscription max QoS.
     * @param subscriptionOptions Subscription options.
     * @param subscriptionId    Optional subscription id.
     */
    SessionSubscription(const std::shared_ptr<ISubscriptionClient>& clientSession, std::shared_ptr<Subscription> subscription, Qos qos,
                        SubscriptionOptions subscriptionOptions, uint32_t subscriptionId = 0);
};

using SSessionSubscription = std::shared_ptr<SessionSubscription>;

} // namespace xmq
