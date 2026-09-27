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
#include "base/MessageProperties.h"
#include "base/Property.h"
#include "base/MessageProperties.h"

#include <gtest/gtest.h>
#include <string>

using namespace std;
using namespace xmq;
using namespace testing;

TEST(MessagePropertyTests, PayloadFormatIndicator)
{
    EXPECT_EQ(propertyToString(Property::PayloadFormatIndicator), "PayloadFormatIndicator");
    EXPECT_EQ(propertyToString(Property::MessageExpiryInterval), "MessageExpiryInterval");
    EXPECT_EQ(propertyToString(Property::ContentType), "ContentType");
    EXPECT_EQ(propertyToString(Property::ResponseTopic), "ResponseTopic");
    EXPECT_EQ(propertyToString(Property::CorrelationData), "CorrelationData");
    EXPECT_EQ(propertyToString(Property::SubscriptionIdentifier), "SubscriptionIdentifier");
    EXPECT_EQ(propertyToString(Property::SessionExpiryInterval), "SessionExpiryInterval");
    EXPECT_EQ(propertyToString(Property::AssignedClientIdentifier), "AssignedClientIdentifier");
    EXPECT_EQ(propertyToString(Property::ServerKeepAlive), "ServerKeepAlive");
    EXPECT_EQ(propertyToString(Property::AuthenticationMethod), "AuthenticationMethod");
    EXPECT_EQ(propertyToString(Property::AuthenticationData), "AuthenticationData");
    EXPECT_EQ(propertyToString(Property::RequestProblemInformation), "RequestProblemInformation");
    EXPECT_EQ(propertyToString(Property::WillDelayInterval), "WillDelayInterval");
    EXPECT_EQ(propertyToString(Property::RequestResponseInformation), "RequestResponseInformation");
    EXPECT_EQ(propertyToString(Property::ResponseInformation), "ResponseInformation");
    EXPECT_EQ(propertyToString(Property::ServerReference), "ServerReference");
    EXPECT_EQ(propertyToString(Property::ReasonString), "ReasonString");
    EXPECT_EQ(propertyToString(Property::ReceiveMaximum), "ReceiveMaximum");
    EXPECT_EQ(propertyToString(Property::TopicAliasMaximum), "TopicAliasMaximum");
    EXPECT_EQ(propertyToString(Property::TopicAlias), "TopicAlias");
    EXPECT_EQ(propertyToString(Property::MaximumQOS), "MaximumQOS");
    EXPECT_EQ(propertyToString(Property::RetainAvailable), "RetainAvailable");
    EXPECT_EQ(propertyToString(Property::UserProperty), "UserProperty");
    EXPECT_EQ(propertyToString(Property::MaximumPacketSize), "MaximumPacketSize");
    EXPECT_EQ(propertyToString(Property::WildcardSubscriptionAvailable), "WildcardSubscriptionAvailable");
    EXPECT_EQ(propertyToString(Property::SubscriptionIdentifierAvailable), "SubscriptionIdentifierAvailable");
    EXPECT_EQ(propertyToString(Property::SharedSubscriptionAvailable), "SharedSubscriptionAvailable");
}

namespace {

/// Round-trip a set of properties the way a message does: encode to wire bytes, decode back.
MessageProperties throughTheWire(const MessageProperties& properties)
{
    sptk::Buffer buffer(properties.expectedSize() + 1);
    auto*        tail = buffer.data();
    properties.write(tail, {});
    buffer.bytes(static_cast<size_t>(tail - buffer.data()));

    MessageProperties decoded;
    decoded.read(buffer.data(), buffer.bytes());
    return decoded;
}

} // namespace

TEST(MessagePropertyTests, RoundTrip)
{
    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::ContentType, "application/json");
    properties->setProperty(Property::ServerKeepAlive, 123);

    UserMessageProperties userProperties;
    userProperties.emplace("key1", "value1");
    userProperties.emplace("key2", "value2");
    properties->setUserProperties(userProperties);

    const auto properties2 = make_shared<MessageProperties>(throughTheWire(*properties));

    string_view strValue;
    EXPECT_TRUE(properties2->getProperty(Property::ContentType, strValue));
    EXPECT_EQ(strValue, "application/json");

    int64_t intValue = 0;
    EXPECT_TRUE(properties2->getProperty(Property::ServerKeepAlive, intValue));
    EXPECT_EQ(intValue, 123);

    EXPECT_EQ(properties2->getUserProperties().size(), 2U);
    EXPECT_EQ(properties2->getUserProperties().at("key1"), "value1");
    EXPECT_EQ(properties2->getUserProperties().at("key2"), "value2");
}

TEST(MessagePropertyTests, RoundTrip_SubscriptionIds)
{
    MessageProperties properties;
    properties.setSubscriptionIds({1, 127, 123456});

    const auto properties2 = throughTheWire(properties);

    EXPECT_EQ(properties2.getSubscriptionIds(), (set<uint32_t> {1, 127, 123456}));
}

TEST(MessagePropertyTests, Validate_ResponseTopicRejectsWildcards)
{
    MessageProperties  properties;
    MessageProperties& baseProperties = properties;
    properties.setProperty(Property::ResponseTopic, "invalid/topic/+");
    EXPECT_EQ(baseProperties.validate(), ReasonCode::ProtocolError);

    properties.setProperty(Property::ResponseTopic, "invalid/topic/#");
    EXPECT_EQ(baseProperties.validate(), ReasonCode::ProtocolError);

    properties.setProperty(Property::ResponseTopic, "valid/topic");
    EXPECT_EQ(baseProperties.validate(), ReasonCode::Success);
}

TEST(MessagePropertyTests, Read_ReplacesExistingState)
{
    MessageProperties source;
    source.setProperty(Property::MaximumPacketSize, 4096);
    source.setProperty(Property::AuthenticationMethod, "token");

    sptk::Buffer encoded(source.expectedSize());
    auto*        tail = encoded.data();
    source.write(tail, {});
    encoded.bytes(tail - encoded.data());

    MessageProperties target;
    target.setProperty(Property::SessionExpiryInterval, 42);
    target.setProperty(Property::ContentType, "old/type");
    target.setUserProperty("leftover", "value");
    target.setSubscriptionIds({99});

    target.read(encoded.data(), encoded.size());

    string_view stringValue;
    int64_t     intValue = 0;
    EXPECT_TRUE(target.getProperty(Property::AuthenticationMethod, stringValue));
    EXPECT_EQ(stringValue, "token");
    EXPECT_TRUE(target.getProperty(Property::MaximumPacketSize, intValue));
    EXPECT_EQ(intValue, 4096);

    EXPECT_FALSE(target.getProperty(Property::SessionExpiryInterval, intValue));
    EXPECT_FALSE(target.getProperty(Property::ContentType, stringValue));
    EXPECT_TRUE(target.getUserProperties().empty());
    EXPECT_TRUE(target.getSubscriptionIds().empty());
}
