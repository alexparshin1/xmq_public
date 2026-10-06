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

#include "LatencyTrace.h"
#include "base/xmq.h"

#include "ReasonCode.h"
#include "SubscriptionIdSet.h"
#include "base/Property.h"
#include <functional>
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief User-defined message properties.
 */
class XMQ_EXPORT UserMessageProperties : public std::unordered_map<std::string, std::string>
{
public:
    using std::unordered_map<std::string, std::string>::unordered_map; ///< The name to value map.
    [[nodiscard]] std::string toString() const;                        ///< Return string representation of user properties.
};

using SUserMessageProperties = std::shared_ptr<UserMessageProperties>;

/**
 * @brief Called once per user property by forEachUserProperty().
 *
 * The views live only for the duration of the call: an implementation is free to point them
 * straight at its own storage rather than hand out copies.
 */
using UserPropertyVisitor = std::function<void(std::string_view name, std::string_view value)>;

class XMQ_EXPORT IMessageProperties
{
public:
    IMessageProperties() = default;
    IMessageProperties(const IMessageProperties&) = default;
    IMessageProperties(IMessageProperties&&) = default;
    IMessageProperties& operator=(const IMessageProperties&) = default;
    IMessageProperties& operator=(IMessageProperties&&) = default;

    /**
     * @brief Destructor.
     * @remarks Virtual because the properties are held and passed as shared_ptr<IMessageProperties>.
     *          It was safe only by accident: make_shared remembers the concrete type, so the right
     *          destructor ran as long as nobody ever created one any other way.
     */
    virtual ~IMessageProperties() = default;

    /**
     * @brief Read message properties from the data buffer.
     * @param data Data buffer.
     * @param length Data size.
     */
    virtual void read(const uint8_t* data, size_t length) = 0;

    /**
     * @brief Write message properties to the data buffer.
     * @param tail Data buffer tail.
     * @param subscriptionIds Subscription ids.
     */
    virtual void write(uint8_t*& tail, const SubscriptionIdSet& subscriptionIds) const = 0;

    /**
     * @brief Return expected binary size, bytes.
     * @return Expected binary size, bytes.
     */
    [[nodiscard]] virtual uint32_t expectedSize() const = 0;

    /**
     * @brief Get property value (integral types only).
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    [[nodiscard]] virtual bool getProperty(Property property, int64_t& value) const = 0;

    /**
     * @brief Get string property value.
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    [[nodiscard]] virtual bool getProperty(Property property, std::string_view& value) const = 0;

    /**
     * @brief Get property value (integral types only) and remove it from properties.
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    [[nodiscard]] virtual bool takeProperty(Property property, int64_t& value) = 0;

    /**
     * @brief Set property value (integral types only).
     * @param property          Property id.
     * @param value             Input value.
     */
    virtual void setProperty(Property property, int64_t value) = 0;

    /**
     * @brief Set the string property value.
     * @tparam T                Data type.
     * @param property          Property id.
     * @param value             Input value.
     */
    virtual void setProperty(Property property, const std::string_view& value) = 0;

    /**
     * @brief Add subscription ID.
     * @param subscriptionId Subscription ID.
     * @return true if subscription ID is added.
     */
    virtual bool addSubscriptionId(uint32_t subscriptionId) = 0;

    /**
     * @brief Set several subscription IDs.
     * @param subscriptionIds Subscription IDs.
     */
    virtual void setSubscriptionIds(const std::set<uint32_t>& subscriptionIds) = 0;

    /**
     * @brief Get subscription IDs.
     * @remarks By value: an implementation is not obliged to keep them in a set.
     * @return Subscription IDs.
     */
    [[nodiscard]] virtual std::set<uint32_t> getSubscriptionIds() const = 0;

    /**
     * @brief Get user-defined properties as a map.
     * @remarks By value, and lossy where a name repeats: an implementation is not obliged to keep
     *          them in a map, and MQTT5 allows the same name more than once. For logging and for
     *          tests; anything on a message path wants forEachUserProperty().
     * @return User-defined properties.
     */
    [[nodiscard]] virtual UserMessageProperties getUserProperties() const = 0;

    /**
     * @brief Visit every user-defined property, in order, repeats included.
     * @param visitor Called once per property with its name and value.
     */
    virtual void forEachUserProperty(const UserPropertyVisitor& visitor) const = 0;

    /**
     * @brief Get the first user-defined property with this name.
     * @remarks The view may point into the properties and is invalidated by the next change.
     * @param name Property name.
     * @return Property value, empty when there is no such property.
     */
    [[nodiscard]] virtual std::string_view getUserProperty(std::string_view name) const = 0;

    /**
     * @brief Set user-defined property.
     * @param name Property name.
     * @param value Property value.
     */
    virtual void setUserProperty(std::string_view name, std::string_view value) = 0;

    /**
     * @brief Set user-defined properties.
     * @param userProperties Properties.
     */
    virtual void setUserProperties(const UserMessageProperties& userProperties) = 0;

    /**
     * @brief Check if the message is expired.
     * @param remainingSeconds Remaining seconds before expiration.
     * @return true if the message is expired, false otherwise.
     */
    virtual bool isExpired(uint32_t& remainingSeconds) const = 0;

    /**
     * @brief Remove property.
     * @param property Property ID.
     * @param propertyType Property Type.
     */
    virtual void removeProperty(const Property property, const PropertyType propertyType) = 0;

    /**
     * @brief Remove every user-defined property with this name, and no other.
     * @param name Property name.
     */
    virtual void removeUserProperty(std::string_view name) = 0;

    /**
     * @brief Remove all properties.
     */
    virtual void clear() = 0;

    /**
     * @brief The properties as text, for a log line.
     *
     * On the interface rather than on the class, and the streaming operator goes through it: the
     * operator used to walk the two maps directly, which obliged every implementation to have two
     * maps to walk. What the properties are made of is the implementation's business.
     */
    [[nodiscard]] virtual std::string toString() const = 0;

    /**
     * @brief Validate string property value.
     * @returns validation result.
     */
    [[nodiscard]] virtual ReasonCode validate(Property, const std::string_view&) const
    {
        return ReasonCode::Success;
    }

    /**
     * @brief Validate all properties.
     */
    [[nodiscard]] virtual ReasonCode validate() const
    {
        return ReasonCode::Success;
    }

    virtual LatencyTrace* getLatencyTrace(bool autoCreate = false) = 0;
    virtual void          setLatencyTrace(const LatencyTrace* latencyTrace) = 0;
};

} // namespace xmq

XMQ_EXPORT std::ostream& operator<<(std::ostream& outputStream, const xmq::IMessageProperties& properties);
