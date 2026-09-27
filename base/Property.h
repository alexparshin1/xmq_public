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

#include <optional>

#include "base/xmq.h"

#include <string>

namespace xmq {

enum class Property : uint8_t
{
    AssignedClientIdentifier = 0x12,
    AuthenticationData = 0x16,
    AuthenticationMethod = 0x15,
    ContentType = 0x03,
    CorrelationData = 0x09,
    MaximumPacketSize = 0x27,
    MaximumQOS = 0x24,
    MessageExpiryInterval = 0x02,
    PayloadFormatIndicator = 0x01,
    ReasonString = 0x1F,
    ReceiveMaximum = 0x21,
    RequestProblemInformation = 0x17,
    RequestResponseInformation = 0x19,
    ResponseInformation = 0x1A,
    ResponseTopic = 0x08,
    RetainAvailable = 0x25,
    ServerKeepAlive = 0x13,
    ServerReference = 0x1C,
    SessionExpiryInterval = 0x11,
    SharedSubscriptionAvailable = 0x2A,
    SubscriptionIdentifier = 0x0B,
    SubscriptionIdentifierAvailable = 0x29,
    TopicAlias = 0x23,
    TopicAliasMaximum = 0x22,
    UserProperty = 0x26,
    WillDelayInterval = 0x18,
    WildcardSubscriptionAvailable = 0x28
};

enum class PropertyType : uint8_t
{
    Integer,
    String,
    User
};

enum class PropertyKind : uint8_t
{
    StringProperty,
    IntegerProperty,
    ByteProperty,
    UserProperty
};

XMQ_EXPORT std::string propertyToString(Property property);

/**
 * @brief The property's name as a command line writes it: "receive-maximum", not "ReceiveMaximum".
 *
 * Derived from propertyToString() rather than listed again, so a property added to one is spelled
 * the same by the other.
 */
XMQ_EXPORT std::string propertyToOptionName(Property property);

/**
 * @brief The property a command line names, or nothing when the name is not one.
 *
 * Accepts the option spelling ("topic-alias-maximum") and the internal one ("TopicAliasMaximum"),
 * because both appear in the documentation and neither is worth being strict about.
 */
[[nodiscard]] XMQ_EXPORT std::optional<Property> propertyFromName(std::string_view name);

constexpr PropertyKind propertyKind(const Property property)
{
    auto kind = PropertyKind::StringProperty;
    using enum Property;
    switch (property)
    {
        case AssignedClientIdentifier:
        case AuthenticationData:
        case AuthenticationMethod:
        case ContentType:
        case CorrelationData:
        case PayloadFormatIndicator:
        case ReasonString:
        case ResponseInformation:
        case ResponseTopic:
        case ServerReference:
            kind = PropertyKind::StringProperty;
            break;
        case MaximumPacketSize:
        case MessageExpiryInterval:
        case ReceiveMaximum:
        case RequestResponseInformation:
        case RequestProblemInformation:
        case SessionExpiryInterval:
        case SubscriptionIdentifier:
        case TopicAlias:
        case TopicAliasMaximum:
        case ServerKeepAlive:
        case WillDelayInterval:
            kind = PropertyKind::IntegerProperty;
            break;
        case MaximumQOS:
        case RetainAvailable:
        case WildcardSubscriptionAvailable:
        case SubscriptionIdentifierAvailable:
        case SharedSubscriptionAvailable:
            kind = PropertyKind::ByteProperty;
            break;
        case UserProperty:
            kind = PropertyKind::UserProperty;
            break;
    }
    return kind;
}

} // namespace xmq
