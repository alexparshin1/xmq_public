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
#include "PropertyValueTypes.h"

using namespace xmq;
using namespace mqtt;

using enum Property;

namespace {
const std::map<Property, PropertyValueType> g_propertyValueTypes = {
    {AssignedClientIdentifier, PropertyValueType::String},
    {AuthenticationData, PropertyValueType::Binary},
    {AuthenticationMethod, PropertyValueType::String},
    {ContentType, PropertyValueType::String},
    {CorrelationData, PropertyValueType::Binary},
    {MaximumPacketSize, PropertyValueType::Uint32},
    {MaximumQOS, PropertyValueType::Uint8},
    {MessageExpiryInterval, PropertyValueType::Uint32},
    {PayloadFormatIndicator, PropertyValueType::Uint8},
    {ReasonString, PropertyValueType::String},
    {ReceiveMaximum, PropertyValueType::Uint16},
    {RequestProblemInformation, PropertyValueType::Uint8},
    {RequestResponseInformation, PropertyValueType::Uint8},
    {ResponseInformation, PropertyValueType::String},
    {ResponseTopic, PropertyValueType::String},
    {RetainAvailable, PropertyValueType::Uint8},
    {ServerKeepAlive, PropertyValueType::Uint16},
    {ServerReference, PropertyValueType::String},
    {SessionExpiryInterval, PropertyValueType::Uint32},
    {SharedSubscriptionAvailable, PropertyValueType::Uint8},
    {SubscriptionIdentifier, PropertyValueType::VarInt},
    {SubscriptionIdentifierAvailable, PropertyValueType::Uint8},
    {TopicAlias, PropertyValueType::Uint16},
    {TopicAliasMaximum, PropertyValueType::Uint16},
    {UserProperty, PropertyValueType::NameValue},
    {WillDelayInterval, PropertyValueType::Uint32},
    {WildcardSubscriptionAvailable, PropertyValueType::Uint8}};

class PropertyValueTypesLoader
{
public:
    PropertyValueTypesLoader()
    {
        PropertyValueTypes::init();
    }
};

[[maybe_unused]] const PropertyValueTypesLoader g_propertyValueTypesLoader;

} // namespace

std::array<PropertyValueType, 256> PropertyValueTypes::m_propertyValueTypes;

void PropertyValueTypes::init()
{
    for (const auto& [property, valueType]: g_propertyValueTypes)
    {
        m_propertyValueTypes[static_cast<uint8_t>(property)] = valueType;
    }
}
