/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "Property.h"
#include <optional>
#include <map>
#include <cctype>

using namespace std;

namespace xmq {

// Write me a function converting property to string
XMQ_EXPORT string propertyToString(const Property property)
{
    switch (property)
    {
        using enum Property;
        case AssignedClientIdentifier:
            return "AssignedClientIdentifier";
        case AuthenticationData:
            return "AuthenticationData";
        case AuthenticationMethod:
            return "AuthenticationMethod";
        case ContentType:
            return "ContentType";
        case CorrelationData:
            return "CorrelationData";
        case MaximumPacketSize:
            return "MaximumPacketSize";
        case MaximumQOS:
            return "MaximumQOS";
        case MessageExpiryInterval:
            return "MessageExpiryInterval";
        case PayloadFormatIndicator:
            return "PayloadFormatIndicator";
        case ReasonString:
            return "ReasonString";
        case ReceiveMaximum:
            return "ReceiveMaximum";
        case RequestProblemInformation:
            return "RequestProblemInformation";
        case RequestResponseInformation:
            return "RequestResponseInformation";
        case ResponseInformation:
            return "ResponseInformation";
        case ResponseTopic:
            return "ResponseTopic";
        case RetainAvailable:
            return "RetainAvailable";
        case ServerKeepAlive:
            return "ServerKeepAlive";
        case ServerReference:
            return "ServerReference";
        case SessionExpiryInterval:
            return "SessionExpiryInterval";
        case SharedSubscriptionAvailable:
            return "SharedSubscriptionAvailable";
        case SubscriptionIdentifier:
            return "SubscriptionIdentifier";
        case SubscriptionIdentifierAvailable:
            return "SubscriptionIdentifierAvailable";
        case TopicAlias:
            return "TopicAlias";
        case TopicAliasMaximum:
            return "TopicAliasMaximum";
        case UserProperty:
            return "UserProperty";
        case WildcardSubscriptionAvailable:
            return "WildcardSubscriptionAvailable";
        case WillDelayInterval:
            return "WillDelayInterval";
    }
    return "Unknown";
}

namespace {

/**
 * @brief "MaximumQOS" -> "maximum-qos", "PayloadFormatIndicator" -> "payload-format-indicator".
 *
 * A dash goes before a capital that follows a small letter, and before the last capital of a run
 * that is followed by a small one - so an abbreviation stays whole instead of becoming "q-o-s".
 */
std::string toOptionName(const std::string& camelCase)
{
    std::string name;
    for (size_t i = 0; i < camelCase.size(); ++i)
    {
        const auto current = camelCase[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(current)))
        {
            const auto previousIsSmall = std::islower(static_cast<unsigned char>(camelCase[i - 1])) != 0;
            const auto nextIsSmall = i + 1 < camelCase.size() &&
                                     std::islower(static_cast<unsigned char>(camelCase[i + 1])) != 0;
            if (previousIsSmall || nextIsSmall)
            {
                name += '-';
            }
        }
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(current)));
    }
    return name;
}

/// Both spellings of every property the switch above knows, built once from that switch.
const std::map<std::string, Property>& propertyNames()
{
    static const auto names = []
    {
        std::map<std::string, Property> byName;
        for (int candidate = 0; candidate <= 0xFF; ++candidate)
        {
            const auto property = static_cast<Property>(candidate);
            if (const auto camelCase = propertyToString(property);
                camelCase != "Unknown")
            {
                byName.emplace(toOptionName(camelCase), property);
                byName.emplace(camelCase, property);
            }
        }
        return byName;
    }();
    return names;
}

} // namespace

XMQ_EXPORT string propertyToOptionName(const Property property)
{
    return toOptionName(propertyToString(property));
}

std::optional<Property> propertyFromName(const std::string_view name)
{
    const auto& names = propertyNames();
    const auto  found = names.find(std::string(name));
    return found == names.end() ? std::nullopt : std::optional {found->second};
}

} // namespace xmq
