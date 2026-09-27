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

#include <base/AckMessage.h>
#include <common/mqtt/PublishMessage.h>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
constexpr auto number_12 = 12;
constexpr auto number_123 = 123;
constexpr auto number_1234 = 1234;
constexpr auto number_12345 = 12345;
constexpr auto number_123456 = 123456;
constexpr auto number_16384 = 16384;
constexpr auto number_456 = 456;
constexpr auto number_1024 = 1024;
} // namespace

TEST(XMQ_Message, AckMessageToString)
{
    const AckMessage message(Message::Type::ConnectAck, number_1234);

    EXPECT_STREQ("ConnectAck ack to id=1234", message.toString().c_str());
}

TEST(XMQ_Message, PublishMessage_CopyCtor)
{
    TopicManager         topicManager;
    mqtt::PublishMessage sourceMessage(topicManager.getTopic("topic1"),
                                       string_view("test", 4),
                                       number_1234, true);
    mqtt::PublishMessage copyMessage(sourceMessage);

    EXPECT_TRUE(sourceMessage.destination() == copyMessage.destination());
    EXPECT_TRUE(Buffer(sourceMessage.payloadData(), sourceMessage.payloadSize()) == Buffer(copyMessage.payloadData(), copyMessage.payloadSize()));
    EXPECT_EQ(sourceMessage.getId(), copyMessage.getId());
    EXPECT_EQ(sourceMessage.isRetain(), copyMessage.isRetain());
    EXPECT_STREQ(sourceMessage.toString().c_str(), copyMessage.toString().c_str());
}

TEST(XMQ_Message, Properties_ToString)
{
    MessageProperties properties;

    properties.setProperty(Property::AuthenticationData, "username=test");
    properties.setProperty(Property::AuthenticationMethod, "password");
    properties.setProperty(Property::ContentType, "json");
    properties.setProperty(Property::CorrelationData, "12345");
    properties.setProperty(Property::MaximumPacketSize, number_16384);
    properties.setProperty(Property::MessageExpiryInterval, number_123);
    properties.setProperty(Property::PayloadFormatIndicator, 1);
    properties.setProperty(Property::ReasonString, "Ok");
    properties.setProperty(Property::ReceiveMaximum, number_1024);
    properties.setProperty(Property::RequestProblemInformation, 1);
    properties.setProperty(Property::RequestResponseInformation, 0);
    properties.setProperty(Property::ResponseTopic, "topic1");
    properties.setProperty(Property::SessionExpiryInterval, number_12345);
    properties.setProperty(Property::TopicAlias, number_12);
    properties.setProperty(Property::TopicAliasMaximum, number_123);

    set<uint32_t> subscriptionIds {number_123, number_456};
    properties.setSubscriptionIds(subscriptionIds);
    // A subscription identifier is one thing however it is set: this lands beside the two above,
    // where it used to sit apart from them as an integer property of its own.
    properties.setProperty(Property::SubscriptionIdentifier, number_123456);

    stringstream stream;
    stream << properties;

    auto propertiesString = stream.str();

    EXPECT_NE(propertiesString.find("AuthenticationData=[username=test]"), string::npos);
    EXPECT_NE(propertiesString.find("AuthenticationMethod=[password]"), string::npos);
    EXPECT_NE(propertiesString.find("CorrelationData=[12345]"), string::npos);
    EXPECT_NE(propertiesString.find("MaximumPacketSize=16384"), string::npos);
    EXPECT_NE(propertiesString.find("MessageExpiryInterval=123"), string::npos);
    EXPECT_NE(propertiesString.find("PayloadFormatIndicator=1"), string::npos);
    EXPECT_NE(propertiesString.find("ReasonString=[Ok]"), string::npos);
    EXPECT_NE(propertiesString.find("ReceiveMaximum=1024"), string::npos);
    EXPECT_NE(propertiesString.find("RequestProblemInformation=1"), string::npos);
    EXPECT_NE(propertiesString.find("RequestResponseInformation=0"), string::npos);
    EXPECT_NE(propertiesString.find("ResponseTopic=[topic1]"), string::npos);
    EXPECT_NE(propertiesString.find("SessionExpiryInterval=12345"), string::npos);
    EXPECT_NE(propertiesString.find("TopicAlias=12"), string::npos);
    EXPECT_NE(propertiesString.find("TopicAliasMaximum=123"), string::npos);

    EXPECT_NE(propertiesString.find("SubscriptionIds=[123,456,123456]"), string::npos);
}
