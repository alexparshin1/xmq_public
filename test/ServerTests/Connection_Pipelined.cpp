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

// A client that does not wait for CONNACK before sending the rest of its work.
//
// MQTT 5, 3.1.4: "Clients are allowed to send further MQTT Control Packets immediately after
// sending a CONNECT packet; Clients need not wait for a CONNACK packet to arrive from the Server."
// So CONNECT and SUBSCRIBE may reach the broker in a single TCP segment, and the broker has to
// answer both. There is nothing the broker can ask of the client here - it cannot require a
// round trip the specification says is optional - so this has to work on the server side.
//
// XMQ's own client does wait, which is why nothing else in the suite covers this shape.

#include "client/MqttClient.h"
#include "common/ConnectMessage.h"
#include "common/GenericProtocols.h"
#include "common/SubscribeMessage.h"
#include "test/ServerTests_Suite.h"

#include <gtest/gtest.h>
#include <sptk5/cnet>

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

class XMQ_PipelinedConnectTests
    : public ServerTests_Suite
{
};

// One CONNECT and one SUBSCRIBE, serialised into a single buffer and handed to a single write().
//
// The single write is the point of the test: two writes would let the broker finish the CONNECT
// before the SUBSCRIBE arrives, which is the case that already works.
Buffer pipelinedConnectAndSubscribe(const string& clientId, const string& topicName,
                                    const bool cleanSession, const ProtocolVersion protocolVersion)
{
    const GenericProtocols protocols(ServerTests_Suite::server()->getTopicManager());
    const auto&            messageWriter = protocols.getProtocol(protocolVersion).messageWriter();

    Buffer buffer;

    ConnectMessage connectMessage(ConnectCredentials(clientId, "user", "secret"), nullptr,
                                  cleanSession, protocolVersion);
    messageWriter->writeConnect(buffer, &connectMessage, nullptr);

    Destinations destinations {Destination(client::MqttClient::getTopic(topicName),
                                           SubscriptionOptions(Qos::Qos1))};
    const SubscribeMessage subscribeMessage(std::move(destinations));
    messageWriter->appendSubscribeToBuffer(buffer, subscribeMessage, 1);

    return buffer;
}

// Reads until both a CONNACK and a SUBACK have been seen, or the timeout runs out.
//
// Only the packet type byte is inspected. Decoding the packets properly would need a session to
// decode them against, and what is being asked here is simply whether the broker answered at all.
bool awaitConnackAndSuback(TCPSocket& socket, const chrono::milliseconds timeout = 2000ms)
{
    constexpr uint8_t connectAckType = 0x20;
    constexpr uint8_t subscribeAckType = 0x90;

    auto       sawConnectAck = false;
    auto       sawSubscribeAck = false;
    const auto deadline = chrono::steady_clock::now() + timeout;

    Buffer received;
    while (chrono::steady_clock::now() < deadline && !(sawConnectAck && sawSubscribeAck))
    {
        if (!socket.readyToRead(100ms))
        {
            continue;
        }

        Buffer chunk;
        if (socket.read(chunk, 1024) == 0)
        {
            break; // peer closed
        }
        received.append(chunk.data(), chunk.bytes());

        // Walk the packets: one byte of type and flags, then a variable-length remaining length.
        size_t offset = 0;
        while (offset < received.bytes())
        {
            const auto packetType = static_cast<uint8_t>(received.data()[offset] & 0xF0);

            size_t   remainingLength = 0;
            size_t   multiplier = 1;
            auto     lengthBytes = size_t {0};
            auto     complete = false;
            while (offset + 1 + lengthBytes < received.bytes())
            {
                const auto lengthByte = static_cast<uint8_t>(received.data()[offset + 1 + lengthBytes]);
                remainingLength += (lengthByte & 0x7FU) * multiplier;
                multiplier *= 128;
                ++lengthBytes;
                if ((lengthByte & 0x80U) == 0)
                {
                    complete = true;
                    break;
                }
            }

            if (!complete || offset + 1 + lengthBytes + remainingLength > received.bytes())
            {
                break; // partial packet; wait for the rest
            }

            sawConnectAck = sawConnectAck || packetType == connectAckType;
            sawSubscribeAck = sawSubscribeAck || packetType == subscribeAckType;

            offset += 1 + lengthBytes + remainingLength;
        }

        if (offset > 0)
        {
            received.erase(0, offset);
        }
    }

    EXPECT_TRUE(sawConnectAck) << "No CONNACK for a pipelined CONNECT";
    return sawConnectAck && sawSubscribeAck;
}

// Publishes one message from a throwaway client and reports whether it reached the pipelined
// subscriber's socket within the timeout.
bool publishReachesSocket(TCPSocket& socket, const string& topicName,
                          const chrono::milliseconds timeout = 2000ms)
{
    constexpr uint8_t publishType = 0x30;

    auto publisher = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());
    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;
    EXPECT_EQ(ReasonCode::Success,
              publisher->connect(Host("localhost", ServerTests_Suite::TestTcpPortNumber),
                                 ConnectCredentials("pipelined-publisher-" + topicName, "user", "secret"),
                                 connectParameters, ProtocolVersion::MqttV5));
    publisher->publish(client::MqttClient::getTopic(topicName), Buffer("payload"), Qos::Qos1);

    const auto deadline = chrono::steady_clock::now() + timeout;
    while (chrono::steady_clock::now() < deadline)
    {
        if (!socket.readyToRead(100ms))
        {
            continue;
        }
        Buffer chunk;
        if (socket.read(chunk, 1024) == 0)
        {
            break;
        }
        for (size_t offset = 0; offset < chunk.bytes(); ++offset)
        {
            if (static_cast<uint8_t>(chunk.data()[offset] & 0xF0) == publishType)
            {
                publisher->disconnect();
                return true;
            }
        }
    }

    publisher->disconnect();
    return false;
}

} // namespace

// A client id the broker has never seen. The session created when the connection was accepted is
// the one that stays, so the SUBSCRIBE that follows the CONNECT is handled by the right session
// whether or not the broker treats the two packets as one batch.
TEST_F(XMQ_PipelinedConnectTests, Subscribe_WithoutWaitingForConnack_NewSession)
{
    const string topicName = "pipelined/new";
    const string clientId = "pipelined-subscriber-new";

    TCPSocket socket;
    socket.open(Host("localhost", TestTcpPortNumber), Socket::OpenMode::CONNECT, true, 5000ms);
    socket.setOption(IPPROTO_TCP, TCP_NODELAY, 1);

    const auto request = pipelinedConnectAndSubscribe(clientId, topicName, true, ProtocolVersion::MqttV5);
    socket.write(request);

    ASSERT_TRUE(awaitConnackAndSuback(socket))
        << "The SUBSCRIBE sent in the same segment as the CONNECT was never answered";

    EXPECT_TRUE(publishReachesSocket(socket, topicName))
        << "The subscription was acknowledged but delivers nothing";

    socket.close();
}

// The same, for a client id the broker already holds a session for. The CONNECT hands that session
// a new connection, and the SUBSCRIBE behind it in the same buffer must be handled by the session
// that took over - not by the throwaway one the connection was accepted with.
TEST_F(XMQ_PipelinedConnectTests, Subscribe_WithoutWaitingForConnack_Takeover)
{
    const string topicName = "pipelined/takeover";
    const string clientId = "pipelined-subscriber-takeover";

    // Leave a session behind for this client id, then abandon its connection without an orderly
    // disconnect, which is what a reconnecting bridge looks like.
    {
        auto first = make_shared<client::MqttClient>(logEngine());
        client::ConnectParameters connectParameters;
        connectParameters.m_cleanSession = false;
        ASSERT_EQ(ReasonCode::Success,
                  first->connect(Host("localhost", TestTcpPortNumber),
                                 ConnectCredentials(clientId, "user", "secret"),
                                 connectParameters, ProtocolVersion::MqttV5));
        first->hangup();
    }

    TCPSocket socket;
    socket.open(Host("localhost", TestTcpPortNumber), Socket::OpenMode::CONNECT, true, 5000ms);
    socket.setOption(IPPROTO_TCP, TCP_NODELAY, 1);

    // Clean session, so the only subscription that can deliver is the one in this very SUBSCRIBE.
    const auto request = pipelinedConnectAndSubscribe(clientId, topicName, true, ProtocolVersion::MqttV5);
    socket.write(request);

    ASSERT_TRUE(awaitConnackAndSuback(socket))
        << "The SUBSCRIBE sent in the same segment as the CONNECT was never answered after a takeover";

    EXPECT_TRUE(publishReachesSocket(socket, topicName))
        << "The subscription was acknowledged but delivers nothing after a takeover";

    socket.close();
}
