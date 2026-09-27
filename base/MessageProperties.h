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

#include "IMessageProperties.h"
#include "InlineVector.h"
#include "common/mqtt/VariableLength.h"

namespace xmq {

/**
 * @brief MQTT5 message properties, held in the form they travel in.
 *
 * The properties are kept as one block of MQTT5 wire bytes plus an index saying where each one
 * starts and how long it is. Nothing is decoded until somebody asks for it, and writing a message
 * copies the block whole.
 *
 * What this replaces: two unordered_maps and a map of user properties. A CONNACK carrying six
 * properties used to cost seven allocations to build - a bucket array and a node per property -
 * and as many frees to destroy, and reading a PUBLISH allocated a std::string for every string
 * property and two for every user property. Here a set that fits inside the object costs none of
 * that, and encoding is a memcpy.
 *
 * Two consequences worth knowing:
 *
 * - **Repeated user properties survive.** MQTT5 allows the same user property name more than once
 *   and its order is significant; the map silently kept only the first. They are now appended in
 *   the order they arrive and written out in that order.
 * - **Properties keep their arrival order on the wire.** The maps wrote all integers, then all
 *   strings, then the user properties. MQTT5 does not require any particular order between
 *   different properties, only between repeated user properties, which is preserved.
 */
class XMQ_EXPORT MessageProperties : public IMessageProperties
{
public:
    /**
     * @brief Default constructor.
     */
    MessageProperties() = default;

    /**
     * @brief Copy constructor.
     */
    MessageProperties(const MessageProperties&) = default;

    /**
     * @brief Move constructor.
     */
    MessageProperties(MessageProperties&&) = default;

    /**
     * @brief Copy assignment.
     * @return this object.
     */
    MessageProperties& operator=(const MessageProperties&) = default;

    /**
     * @brief Move assignment.
     * @return this object.
     */
    MessageProperties& operator=(MessageProperties&&) = default;

    /**
     * @brief Destructor.
     */
    ~MessageProperties() override = default;

    /**
     * @brief Get property value (integral types only).
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    bool getProperty(Property property, int64_t& value) const override;

    /**
     * @brief Get string property value.
     * @remarks The view points into this object and is invalidated by the next change to it.
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    bool getProperty(Property property, std::string_view& value) const override;

    /**
     * @brief Get property value (integral types only) and remove it from properties.
     * @param property          Property id.
     * @param value             Output value.
     * @return true if property found, false otherwise.
     */
    [[nodiscard]] bool takeProperty(Property property, int64_t& value) override;

    /**
     * @brief Set property value (integral types only).
     * @param property          Property id.
     * @param value             Input value.
     */
    void setProperty(Property property, int64_t value) override;

    /**
     * @brief Set the string property value.
     * @param property          Property id.
     * @param value             Input value.
     */
    void setProperty(Property property, const std::string_view& value) override;

    /**
     * @brief Add subscription ID.
     * @remarks Stored as the SubscriptionIdentifier property it is, in the block with the rest.
     * @param subscriptionId Subscription ID.
     * @return true if it was added, false if the message already carried it.
     */
    bool addSubscriptionId(uint32_t subscriptionId) override;

    /**
     * @brief Set several subscription IDs, replacing any already there.
     * @param subscriptionIds Subscription IDs.
     */
    void setSubscriptionIds(const std::set<uint32_t>& subscriptionIds) override;

    /**
     * @brief Get subscription IDs.
     * @remarks Built on the spot from the block. For logging, for tests, and for the SUBSCRIBE
     *          path, which reads one of them; nothing on a message path calls it.
     * @return Subscription IDs.
     */
    [[nodiscard]] std::set<uint32_t> getSubscriptionIds() const override;

    /**
     * @brief Get user-defined properties as a map.
     * @remarks Built on the spot, and lossy: a name that appears more than once keeps only its
     *          first value, because a map cannot hold the rest. For logging and for tests. Code on
     *          a message path wants forEachUserProperty(), which sees them all, in order.
     * @return User-defined properties.
     */
    [[nodiscard]] UserMessageProperties getUserProperties() const override;

    /**
     * @brief Visit every user-defined property, in the order it arrived, repeats included.
     * @param visitor Called once per property with its name and value.
     */
    void forEachUserProperty(const UserPropertyVisitor& visitor) const override;

    /**
     * @brief Get the first user-defined property with this name.
     * @remarks The view points into this object and is invalidated by the next change to it.
     * @param name Property name.
     * @return Property value, empty when there is no such property.
     */
    [[nodiscard]] std::string_view getUserProperty(std::string_view name) const override;

    /**
     * @brief Add a user-defined property.
     * @remarks Appends: an existing property of the same name is kept, as MQTT5 requires.
     * @param name Property name.
     * @param value Property value.
     */
    void setUserProperty(std::string_view name, std::string_view value) override;

    /**
     * @brief Replace all user-defined properties with these.
     * @param userProperties Properties.
     */
    void setUserProperties(const UserMessageProperties& userProperties) override;

    [[nodiscard]] LatencyTrace* getLatencyTrace(bool autoCreate = false) override;

    void setLatencyTrace(const LatencyTrace* latencyTrace) override;

    /**
     * @brief Check if the message is expired.
     * @param remainingSeconds Remaining seconds before expiration.
     * @return true if the message is expired, false otherwise.
     */
    bool isExpired(uint32_t& remainingSeconds) const override
    {
        if (m_expirationTimeStamp.zero())
        {
            return false;
        }

        // Rounded up, not truncated. Expiry is carried in whole seconds, so any message with less
        // than a second left truncates to zero - and would be treated as expired while it still
        // has most of a second to live. Rounding up errs towards delivering, which is the safe
        // direction: the cost is at most one second of extra life, against silently dropping a
        // message that has not expired. It also keeps a one-second expiry usable at all.
        if (const auto remaining = getExpirationTimeStamp() - sptk::DateTime::Now();
            remaining > sptk::DateTime::duration::zero())
        {
            remainingSeconds = static_cast<uint32_t>(std::chrono::ceil<std::chrono::seconds>(remaining).count());
            return false;
        }
        return true;
    }

    /**
     * @brief The properties as text, for a log line.
     */
    [[nodiscard]] std::string toString() const override;

    /**
     * @brief Remove property.
     * @param property Property ID.
     * @param propertyType Property Type.
     */
    void removeProperty(Property property, PropertyType propertyType) override;

    /**
     * @brief Remove all properties.
     */
    void clear() override;

    /**
     * @brief Read properties from the buffer.
     * @param data              Buffer to read the properties.
     * @param length            Length of the buffer.
     */
    void read(const uint8_t* data, size_t length) override;

    /**
     * @brief Write properties into the buffer.
     * @param tail            Buffer to write the properties.
     * @param subscriptionIds Set of subscription ids.
     */
    void write(uint8_t*& tail, const SubscriptionIdSet& subscriptionIds) const override;

    /**
     * @brief Get the expected size for writing these properties into the buffer.
     * @return Expected write size.
     */
    [[nodiscard]] uint32_t expectedSize() const override;

    /**
     * @brief Validate all properties.
     */
    [[nodiscard]] ReasonCode validate() const override;

    /**
     * @brief Copy every property of another set into this one.
     * @remarks What a set already has of the same name is replaced, except user properties, which
     *          are appended - the same rule the individual setters follow.
     * @param other Properties to copy in.
     */
    void merge(const MessageProperties& other);

    /**
     * @brief Write property as the variable length integer.
     * @remarks The output buffer tail will be advanced by the number of bytes written.
     * @param property          Property.
     * @param tail              Output buffer tail.
     * @param value             Value to be written.
     */
    static void writeVariableLengthIntegerProperty(Property property, uint8_t*& tail, const uint64_t value)
    {
        *tail++ = static_cast<uint8_t>(property);
        mqtt::VariableLength::write(value, tail);
    }

protected:
    [[nodiscard]] const sptk::DateTime& getExpirationTimeStamp() const
    {
        return m_expirationTimeStamp;
    }

    void setExpirationTimeStamp(const sptk::DateTime& expirationTimeStamp)
    {
        m_expirationTimeStamp = expirationTimeStamp;
    }

private:
    /**
     * @brief Where one property sits in the block of wire bytes.
     *
     * The offset points at the identifier byte and the length covers the identifier and its
     * payload, so a property is copied out, or overwritten, as one run of bytes.
     */
    struct Entry
    {
        Property id;      ///< Property identifier.
        uint32_t offset;  ///< Offset of the identifier byte within the block.
        uint32_t length;  ///< Identifier byte plus payload, bytes.
    };

    /// Enough for the six properties of a CONNACK, and for a PUBLISH with a topic alias and a
    /// modest user property, which is what nearly every message on a busy broker carries.
    static constexpr size_t inlineBytes = 96;
    static constexpr size_t inlineEntries = 6;

    [[nodiscard]] const Entry* findEntry(Property property) const;
    [[nodiscard]] size_t       findEntryPosition(Property property) const;

    [[nodiscard]] int64_t          decodeInteger(const Entry& entry) const;
    [[nodiscard]] std::string_view decodeString(const Entry& entry) const;
    void                           decodeUserProperty(const Entry& entry, std::string_view& name,
                                                      std::string_view& value) const;

    void appendInteger(Property property, int64_t value);
    void appendString(Property property, std::string_view value);
    void eraseEntry(size_t position);

    /**
     * @brief How many bytes this property's payload occupies, starting at the identifier byte.
     * @param property   Property identifier.
     * @param payload    First byte after the identifier.
     * @param available  Bytes left in the block after the identifier.
     * @return Payload length; throws when the block ends inside the property.
     */
    [[nodiscard]] static size_t payloadLength(Property property, const uint8_t* payload, size_t available);

    /**
     * @brief Encoded length of an integer property, identifier byte included.
     */
    [[nodiscard]] static size_t encodedIntegerLength(Property property, int64_t value);

    InlineVector<uint8_t, inlineBytes> m_data;   ///< The properties, as they go on the wire.
    InlineVector<Entry, inlineEntries> m_index;  ///< Where each of them starts. Also its liveness.
    sptk::DateTime                     m_expirationTimeStamp;

    /**
     * @brief Validate string property value.
     * @param property          Property.
     * @param value             Property value.
     */
    [[nodiscard]] ReasonCode validate(Property property, const std::string_view& value) const override;
};

/**
 * @brief Set a property from the text a scenario or a command line wrote.
 *
 * The name is the option spelling ("receive-maximum", "user-property") or the internal one; the
 * value is text, converted according to what the property holds. A user property's value is itself
 * a pair, "name=value", split at the first '='.
 *
 * Lives here because both the command line and the scenario file need it, and they are built into
 * different libraries.
 *
 * @param properties    Receives the property.
 * @param name          Property name.
 * @param value         Property value as text.
 * @throws sptk::Exception when the name is not a property, or the text does not fit its type.
 */
XMQ_EXPORT void setPropertyFromText(MessageProperties& properties, std::string_view name,
                                    std::string_view value);

using SMessageProperties = std::shared_ptr<IMessageProperties>;

} // namespace xmq
