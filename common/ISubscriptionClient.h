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

#include "PublishMessage.h"
#include "SubscriptionOptions.h"
#include "storage/Storage.h"

namespace xmq {

class Subscription;
class Storage;

using SStorage = std::shared_ptr<Storage>;
using MessageCallback = std::function<void(const SMessage& message)>;
using PublishMessageCallback = std::function<void(const SPublishMessage& message)>;

/**
 * @interface ISubscriptionClient
 * @brief Interface for implementing subscription-based client services.
 *
 * Provides an abstraction for clients that need to subscribe to and manage
 * messages.
 */
class ISubscriptionClient
{
public:
    virtual ~ISubscriptionClient() = default;

    /**
     * @brief Retrieves the unique identifier for the client.
     *
     * Provides a method to get the client's unique ID.
     *
     * @return The unique client identifier as a string.
     */
    [[nodiscard]] virtual const std::string& getClientId() const = 0;

    /**
     * @brief Retrieves the unique identifier for the client (unlocked version).
     * @return The unique client identifier as a string.
     */
    [[nodiscard]] virtual std::string_view getClientIdUnlocked() const = 0;

    /**
     * @brief Retrieves the username associated with the client.
     * @return The username as a constant reference to a string.
     */
    [[nodiscard]] virtual std::string_view getUsername() const = 0;

    /**
     * @brief Retrieves the database record ID associated with the client session.
     * @return The optional session record ID (for persistent objects) as a RecordId type.
     */
    [[nodiscard]] virtual RecordId recordId() const = 0;

    /**
     * @brief Represents a system for managing and storing data.
     * @return The optional storage (for persistent objects) where the object is stored.
     */
    [[nodiscard]] virtual SStorage storage() const = 0;

    /**
     * @brief Register subscription.
     * @param subscription Subscription.
     * @param qos Subscription QOS.
     * @param subscriptionOptions Subscription options.
     */
    virtual void subscribedTo(const std::shared_ptr<Subscription>& subscription, Qos qos, SubscriptionOptions subscriptionOptions) = 0;

    /**
     * @brief Remove the subscription from the client.
     */
    virtual void unsubscribedFrom(const Subscription&)
    {
    }

    virtual MessageId postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet& subscriptionIds, bool retain) = 0;

    virtual void onMessage(MessageCallback messageCallback) = 0;

    [[nodiscard]] virtual std::string bridgeOrigin() const = 0;

    virtual void setBridgeOrigin(const std::string& bridgeOrigin) = 0;
};

} // namespace xmq
