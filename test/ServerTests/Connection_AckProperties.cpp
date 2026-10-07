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

// What the CONNACK says about the server, when the client has said something about itself.
//
// MQTT 5, 3.2.2.3: Receive Maximum, Maximum Packet Size and Topic Alias Maximum in a CONNACK are
// the *server's* limits - how many unacknowledged publications it will take, how large a packet it
// will accept, how high a topic alias it understands. The client states its own in the CONNECT, and
// the two are unrelated.
//
// The broker used to walk every integer property of the CONNECT and copy each into the CONNACK on
// the way past, so a client that asked for Receive Maximum 10 was told the server accepts 10 - and
// throttled itself to ten in flight where 32768 were available. Nothing caught it, because the load
// generator sends no CONNECT properties at all: every measurement took the one path where the fault
// cannot appear. Hence this test, which sends them.

#include "common/ConnectMessage.h"
#include "common/GenericProtocols.h"
#include "client/MqttClient.h"
#include "test/SubscribeAndWait.h"
#include "test/ServerTests_Suite.h"

#include <gtest/gtest.h>
#include <sptk5/cnet>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

class XMQ_ConnectAckPropertiesTests
    : public ServerTests_Suite
{
};

/// What the client will claim for itself - deliberately nothing like the server's own numbers.
constexpr int64_t clientReceiveMaximum = 10;
constexpr int64_t clientMaximumPacketSize = 1024;
constexpr int64_t clientTopicAliasMaximum = 7;

Buffer connectDeclaringClientLimits(const string& clientId)
{
    using enum Property;

    const GenericProtocols protocols(ServerTests_Suite::server()->getTopicManager());
    const auto&            messageWriter = protocols.getProtocol(ProtocolVersion::MqttV5).messageWriter();

    ConnectMessage connectMessage(ConnectCredentials(clientId, "user", "secret"), nullptr, true,
                                  ProtocolVersion::MqttV5, 60);
    auto properties = make_shared<MessageProperties>();
    properties->setProperty(ReceiveMaximum, clientReceiveMaximum);
    properties->setProperty(MaximumPacketSize, clientMaximumPacketSize);
    properties->setProperty(TopicAliasMaximum, clientTopicAliasMaximum);
    connectMessage.setProperties(properties);

    Buffer buffer;
    messageWriter->writeConnect(buffer, &connectMessage, nullptr);
    return buffer;
}

/// The CONNACK's property block, decoded into property id -> value. Integer properties only.
map<Property, int64_t> readConnectAckProperties(TCPSocket& socket)
{
    constexpr uint8_t connectAckType = 0x20;

    Buffer     received;
    const auto deadline = chrono::steady_clock::now() + 3000ms;
    while (chrono::steady_clock::now() < deadline)
    {
        if (!socket.readyToRead(100ms))
        {
            continue;
        }
        Buffer chunk(1024);
        if (socket.read(chunk, 1024) == 0)
        {
            break;
        }
        received.append(chunk.data(), chunk.bytes());
        if (received.bytes() > 4 && (static_cast<uint8_t>(received.data()[0]) & 0xF0U) == connectAckType)
        {
            break;
        }
    }

    map<Property, int64_t> properties;
    const auto*            bytes = std::bit_cast<const uint8_t*>(received.data());
    if (received.bytes() < 5 || (bytes[0] & 0xF0U) != connectAckType)
    {
        return properties; // no CONNACK: the assertions below will say so
    }

    // Fixed header, then acknowledge flags and the reason code, then the property block.
    size_t offset = 1;
    while (offset < received.bytes() && (bytes[offset] & 0x80U) != 0)
    {
        ++offset;
    }
    ++offset;
    offset += 2;

    const auto propertiesLength = static_cast<size_t>(bytes[offset]);
    ++offset;

    // Only the widths this test looks at; anything else ends the walk rather than guessing a size.
    const map<Property, size_t> widths {
        {Property::ReceiveMaximum, 2}, {Property::TopicAliasMaximum, 2}, {Property::MaximumPacketSize, 4},
        {Property::RetainAvailable, 1}, {Property::WildcardSubscriptionAvailable, 1},
        {Property::SubscriptionIdentifierAvailable, 1}, {Property::MaximumQOS, 1}};

    const auto end = offset + propertiesLength;
    while (offset < end && offset < received.bytes())
    {
        const auto property = static_cast<Property>(bytes[offset]);
        ++offset;
        const auto width = widths.find(property);
        if (width == widths.end())
        {
            break;
        }
        int64_t value = 0;
        for (size_t i = 0; i < width->second; ++i)
        {
            value = (value << 8) | bytes[offset + i];
        }
        offset += width->second;
        properties[property] = value;
    }
    return properties;
}

TEST_F(XMQ_ConnectAckPropertiesTests, TheAckReportsTheServersLimitsAndNotTheClients)
{
    using enum Property;

    TCPSocket socket;
    socket.open(Host("localhost", TestTcpPortNumber), Socket::OpenMode::CONNECT, true, 5000ms);

    const auto connect = connectDeclaringClientLimits("ack-properties");
    socket.write(connect.data(), connect.bytes());

    const auto acknowledged = readConnectAckProperties(socket);
    socket.close();

    ASSERT_FALSE(acknowledged.empty()) << "the broker sent no CONNACK, or none this test could read";

    const auto& settings = *server()->getSettings();
    EXPECT_EQ(settings.m_queue_limits.m_max_inflight_messages.asInteger(), acknowledged.at(ReceiveMaximum));
    EXPECT_EQ(settings.m_server_limits.m_max_packet_size.asInteger(), acknowledged.at(MaximumPacketSize));
    EXPECT_EQ(settings.m_server_limits.m_max_topic_alias.asInteger(), acknowledged.at(TopicAliasMaximum));

    // Said separately, because this is the failure that was actually there: not a wrong number, but
    // the client's own number handed back to it.
    EXPECT_NE(clientReceiveMaximum, acknowledged.at(ReceiveMaximum)) << "the client's Receive Maximum was echoed";
    EXPECT_NE(clientMaximumPacketSize, acknowledged.at(MaximumPacketSize)) << "the client's Maximum Packet Size was echoed";
    EXPECT_NE(clientTopicAliasMaximum, acknowledged.at(TopicAliasMaximum)) << "the client's Topic Alias Maximum was echoed";
}

/**
 * The client keeps to the Receive Maximum the server announces (MQTT 5 section 4.9).
 *
 * Setup: The broker announces Receive Maximum 3. A client connects with MQTT 5 and publishes 200
 * QoS 1 messages back to back to a subscriber.
 * Verification: The client's in-flight limit is the server's 3, and every message still arrives -
 * the window is kept, and nothing is left waiting behind it. A broker that enforces the limit, as
 * HiveMQ does, disconnected every publisher of a load test before this.
 */
TEST_F(XMQ_ConnectAckPropertiesTests, TheClientKeepsToTheServersReceiveMaximum)
{
    auto&      limit = server()->getSettings()->m_queue_limits.m_max_inflight_messages;
    const auto previous = limit.asInteger();
    limit = 3;

    const string publisherId = "receive-maximum-publisher";
    const string subscriberId = "receive-maximum-subscriber";
    const string topicName = "test/receive-maximum";
    constexpr auto count = 200;

    atomic_int received {0};
    const auto subscriber = make_shared<client::MqttClient>(logEngine());
    subscriber->onMessage([&received](const SPublishMessage&) { ++received; });
    ASSERT_EQ(ReasonCode::Success, subscriber->connect(Host("localhost", TestTcpPortNumber),
                                                       ConnectCredentials(subscriberId, "user", "secret"),
                                                       {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    ASSERT_TRUE(test::subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1))));

    const auto publisher = make_shared<client::MqttClient>(logEngine());
    ASSERT_EQ(ReasonCode::Success, publisher->connect(Host("localhost", TestTcpPortNumber),
                                                      ConnectCredentials(publisherId, "user", "secret"),
                                                      {.m_cleanSession = true}, ProtocolVersion::MqttV5));
    limit = previous;

    EXPECT_EQ(3, publisher->inflightLimit()) << "the server's Receive Maximum was not taken";

    for (auto i = 0; i < count; ++i)
    {
        publisher->publish(topicName, format("message {}", i), Qos::Qos1);
    }

    const auto deadline = chrono::steady_clock::now() + 10s;
    while (received < count && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(count, received.load());

    publisher->disconnect();
    subscriber->disconnect();
}

} // namespace
