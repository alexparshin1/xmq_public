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

#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ServerTests.h"

#include <sptk5/wsdl/protocol/WSWebSocketStream.h>
#include <sptk5/wsdl/protocol/WSWebSocketsProtocol.h>

#include <cstring>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// A port of its own for the WebSocket listener, clear of the suite's other ports and of the
/// ephemeral range the tuned test hosts hand out as source ports.
constexpr uint16_t webSocketPort = 1879;

/// The key from RFC 6455 section 1.3, whose accept value the test can compare against.
constexpr const char* rfcKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr const char* rfcAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

void appendRemainingLength(Buffer& packet, size_t length)
{
    do
    {
        auto byte = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0)
        {
            byte |= 0x80;
        }
        packet.append(byte);
    }
    while (length > 0);
}

void appendString(Buffer& packet, const string& value)
{
    packet.append(static_cast<uint8_t>(value.size() >> 8));
    packet.append(static_cast<uint8_t>(value.size() & 0xFF));
    packet.append(reinterpret_cast<const uint8_t*>(value.c_str()), value.size());
}

/// A MQTT 3.1.1 CONNECT with the clean-session flag, a username and a password.
Buffer connectPacket(const string& clientId, const string& user = "user", const string& password = "secret")
{
    Buffer body;
    appendString(body, "MQTT");
    body.append(static_cast<uint8_t>(0x04)); // protocol level 3.1.1
    body.append(static_cast<uint8_t>(0xC2)); // clean session, username, password
    body.append(static_cast<uint8_t>(0x00));
    body.append(static_cast<uint8_t>(0x3C)); // keep alive, 60 seconds
    appendString(body, clientId);
    appendString(body, user);
    appendString(body, password);

    Buffer packet;
    packet.append(static_cast<uint8_t>(0x10));
    appendRemainingLength(packet, body.bytes());
    packet.append(body);
    return packet;
}

Buffer subscribePacket(const uint16_t packetId, const string& topic)
{
    Buffer body;
    body.append(static_cast<uint8_t>(packetId >> 8));
    body.append(static_cast<uint8_t>(packetId & 0xFF));
    appendString(body, topic);
    body.append(static_cast<uint8_t>(0x00)); // requested QoS 0

    Buffer packet;
    packet.append(static_cast<uint8_t>(0x82));
    appendRemainingLength(packet, body.bytes());
    packet.append(body);
    return packet;
}

Buffer publishPacket(const string& topic, const Buffer& payload)
{
    Buffer body;
    appendString(body, topic);
    body.append(payload);

    Buffer packet;
    packet.append(static_cast<uint8_t>(0x30)); // PUBLISH, QoS 0
    appendRemainingLength(packet, body.bytes());
    packet.append(body);
    return packet;
}

/// Takes one whole MQTT packet off the front of the buffer, or says more bytes are needed.
bool takePacket(Buffer& incoming, Buffer& packet)
{
    if (incoming.bytes() < 2)
    {
        return false;
    }

    size_t length = 0;
    size_t multiplier = 1;
    size_t offset = 1;
    while (true)
    {
        if (offset >= incoming.bytes())
        {
            return false;
        }
        const auto byte = incoming[offset++];
        length += (byte & 0x7F) * multiplier;
        if ((byte & 0x80) == 0)
        {
            break;
        }
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128)
        {
            throw runtime_error("malformed remaining length");
        }
    }

    const size_t total = offset + length;
    if (incoming.bytes() < total)
    {
        return false;
    }

    packet.set(incoming.data(), total);
    incoming.erase(0, total);
    return true;
}

/**
 * @brief An MQTT client that reaches the broker through a WebSocket upgrade and frames.
 *
 * The MQTT packets here are written byte by byte rather than built with the broker's own writer: a
 * test that shares its message encoding with the code under test mostly proves the two agree.
 */
class WebSocketClient
{
public:
    WebSocketClient()
        : m_stream(WSWebSocketStream::Role::Client)
    {
    }

    ~WebSocketClient()
    {
        m_socket.close();
    }

    /**
     * @brief Sends the upgrade request and reads the reply.
     * @return The head of the reply, without the body.
     */
    String upgrade(const String& subprotocol = "mqtt")
    {
        m_socket.open(Host("127.0.0.1", webSocketPort), Socket::OpenMode::CONNECT, true, chrono::milliseconds(5000));

        String request = "GET /mqtt HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: ";
        request += rfcKey;
        request += "\r\nSec-WebSocket-Version: 13\r\n";
        if (!subprotocol.empty())
        {
            request += "Sec-WebSocket-Protocol: ";
            request += subprotocol;
            request += "\r\n";
        }
        request += "\r\n";
        m_socket.write(request);

        Buffer response;
        while (true)
        {
            const auto end = String(response.c_str(), response.bytes()).find("\r\n\r\n");
            if (end != string::npos)
            {
                const auto head = String(response.c_str(), end);
                const auto body = response.bytes() - end - 4;
                if (body > 0 && head.starts_with("HTTP/1.1 101"))
                {
                    // Framed bytes that arrived in the same read. Nothing precedes the CONNECT,
                    // but they belong to the transport all the same. A refusal carries an HTML
                    // body instead, which is text rather than frames and is simply dropped.
                    m_stream.feed(response.data() + end + 4, body);
                }
                return head;
            }

            Buffer chunk;
            const auto received = readRaw(chunk, chrono::milliseconds(5000));
            if (received == 0)
            {
                throw runtime_error("no reply to the upgrade request");
            }
            response.append(chunk.data(), received);
        }
    }

    /// Sends MQTT bytes inside one frame.
    void send(const Buffer& payload)
    {
        if (!payload.empty())
        {
            m_stream.write(payload.data(), payload.bytes());
        }
        flush();
    }

    /// Waits for a whole MQTT packet, or returns false when the stream ended first.
    bool receive(Buffer& packet, const chrono::milliseconds timeout = chrono::milliseconds(5000))
    {
        const auto deadline = chrono::steady_clock::now() + timeout;
        while (true)
        {
            if (takePacket(m_pending, packet))
            {
                return true;
            }
            if (chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            if (m_stream.available() > 0)
            {
                uint8_t chunk[64 * 1024];
                const auto read = m_stream.read(chunk, sizeof(chunk));
                m_pending.append(chunk, read);
                continue;
            }
            Buffer chunk;
            readSome(chunk, chrono::milliseconds(200));
        }
    }

    /// Sends a close frame and waits for the peer's answer.
    void close(const uint16_t code = 1000)
    {
        m_stream.close(code, "done");
        flush();

        const auto deadline = chrono::steady_clock::now() + chrono::milliseconds(5000);
        while (!m_stream.closeReceived() && chrono::steady_clock::now() < deadline)
        {
            Buffer chunk;
            readSome(chunk, chrono::milliseconds(200));
        }
    }

    TCPSocket& socket()
    {
        return m_socket;
    }

    WSWebSocketStream& stream()
    {
        return m_stream;
    }

private:
    TCPSocket         m_socket;
    WSWebSocketStream m_stream;
    Buffer            m_pending; ///< MQTT bytes already unwrapped from frames.

    /// Reads whatever has arrived, without interpreting it: during the handshake the bytes are
    /// HTTP, and handing them to the frame parser would read the request line as a frame header.
    size_t readRaw(Buffer& chunk, const chrono::milliseconds wait)
    {
        if (!m_socket.readyToRead(wait))
        {
            return 0;
        }
        return m_socket.read(chunk, 64 * 1024);
    }

    /// Reads and feeds the frames in what arrived.
    size_t readSome(Buffer& chunk, const chrono::milliseconds wait)
    {
        const auto received = readRaw(chunk, wait);
        if (received > 0)
        {
            m_stream.feed(chunk.data(), received);
        }
        return received;
    }

    void flush()
    {
        while (m_stream.hasOutput())
        {
            m_socket.write(m_stream.output());
            m_stream.consumeOutput(m_stream.output().bytes());
        }
    }
};

} // namespace

/**
 * @brief Server tests that also serve MQTT over WebSocket.
 *
 * The shared test server is configured from the suite's template, which knows the two listeners
 * every other test needs. The WebSocket one is added here instead, so a transport test cannot
 * change what the rest of the suite talks to.
 */
class XMQ_WebSocketTests : public XMQ_ServerTests
{
public:
    void SetUp() override
    {
        XMQ_ServerTests::SetUp();

        // Once per server, not once per test: FastTCPServer refuses a second listener on a port it
        // already serves, and the fixture creates the server again only after stopping it.
        if (m_serverOfListener.lock() != server())
        {
            server()->addListener(ServerConnection::Type::WS, Host("127.0.0.1", webSocketPort), 1);
            m_serverOfListener = server();
        }
    }

private:
    static inline std::weak_ptr<Server> m_serverOfListener;
};

/**
 * The listener answers the upgrade request the way RFC 6455 asks: 101, the accept value the
 * specification derives from the example key, and the mqtt subprotocol echoed back.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_Upgrade)
{
    WebSocketClient client;
    const auto      reply = client.upgrade();

    EXPECT_TRUE(reply.starts_with("HTTP/1.1 101")) << reply.c_str();
    EXPECT_NE(string::npos, string(reply.c_str()).find(string("Sec-WebSocket-Accept: ") + rfcAccept))
        << reply.c_str();
    EXPECT_NE(string::npos, string(reply.c_str()).find("Sec-WebSocket-Protocol: mqtt")) << reply.c_str();

    client.close();
}

/**
 * An upgrade request without the mqtt subprotocol is refused rather than answered: a client that
 * offered something else would go on framing its own protocol.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_RejectsAnotherSubprotocol)
{
    WebSocketClient client;
    const auto      reply = client.upgrade("not-mqtt");

    EXPECT_TRUE(reply.starts_with("HTTP/1.1 400")) << reply.c_str();
    EXPECT_EQ(string::npos, string(reply.c_str()).find("101"));
}

/**
 * A CONNECT sent inside a frame is answered with a CONNACK inside a frame: the broker reads MQTT
 * through the transport and writes it back the same way.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_Connect)
{
    WebSocketClient client;
    client.upgrade();
    client.send(connectPacket("ws-connect-client"));

    Buffer connack;
    ASSERT_TRUE(client.receive(connack));
    ASSERT_GE(connack.bytes(), 4U);
    EXPECT_EQ(0x20, static_cast<int>(connack[0]));
    EXPECT_EQ(0x00, static_cast<int>(connack[3])); // connection accepted

    client.close();
}

/**
 * A publish larger than 125 bytes - the length at which a frame header changes shape - arrives
 * whole. This is the case a client-only test cannot cover: the broker both decodes the frame and
 * builds one.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_LargePublish)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const string topic = "ws/" + topicName;

    WebSocketClient client;
    client.upgrade();
    client.send(connectPacket("ws-publish-client"));

    Buffer connack;
    ASSERT_TRUE(client.receive(connack));
    ASSERT_EQ(0x00, static_cast<int>(connack[3]));

    client.send(subscribePacket(1, topic));
    Buffer suback;
    ASSERT_TRUE(client.receive(suback));
    EXPECT_EQ(0x90, static_cast<int>(suback[0]));
    EXPECT_EQ(0x00, static_cast<int>(suback[4])); // granted QoS 0

    Buffer payload(300);
    for (size_t i = 0; i < payload.bytes(); ++i)
    {
        payload[i] = static_cast<uint8_t>('a' + i % 26);
    }
    client.send(publishPacket(topic, payload));

    Buffer delivered;
    ASSERT_TRUE(client.receive(delivered));

    // The delivered packet is the publisher's own PUBLISH, with the topic in front of the payload.
    ASSERT_GT(delivered.bytes(), payload.bytes());
    EXPECT_EQ(0x30, static_cast<int>(delivered[0]) & 0xF0);
    const size_t topicLength = (delivered[2] << 8) | delivered[3];
    ASSERT_EQ(topicLength, topic.size());
    ASSERT_EQ(0, memcmp(delivered.data() + 4, topic.c_str(), topicLength));
    ASSERT_EQ(payload.bytes(), delivered.bytes() - 4 - topicLength);
    EXPECT_EQ(0, memcmp(delivered.data() + 4 + topicLength, payload.data(), payload.bytes()));

    client.close();
}

/**
 * A ping is answered by the transport itself, with the same payload, and the MQTT session never
 * sees it - which is what keeps a browser's keep-alive from being read as a malformed packet.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_Ping)
{
    WebSocketClient client;
    client.upgrade();

    Buffer ping;
    WSWebSocketsMessage::encode(reinterpret_cast<const uint8_t*>("keepalive"), 9, WSWebSocketsMessage::OpCode::PING,
                                true, ping, true);
    client.socket().write(ping);

    Buffer incoming;
    const auto deadline = chrono::steady_clock::now() + chrono::milliseconds(5000);
    while (chrono::steady_clock::now() < deadline)
    {
        if (!client.socket().readyToRead(chrono::milliseconds(200)))
        {
            continue;
        }
        client.socket().read(incoming, 64 * 1024);
        break;
    }

    WSWebSocketsMessage pong;
    ASSERT_GT(incoming.bytes(), 0U) << "the broker did not answer the ping";
    ASSERT_EQ(pong.decode(incoming.data(), incoming.bytes()), incoming.bytes());
    EXPECT_EQ(WSWebSocketsMessage::OpCode::PONG, pong.opcode());
    EXPECT_EQ("keepalive", string(pong.payload().c_str(), pong.payload().bytes()));
}

/**
 * A close frame ends the session, and the broker echoes the close code before letting go.
 */
TEST_F(XMQ_WebSocketTests, WebSocket_Close)
{
    WebSocketClient client;
    client.upgrade();
    client.send(connectPacket("ws-close-client"));

    Buffer connack;
    ASSERT_TRUE(client.receive(connack));

    client.close(1001);
    EXPECT_TRUE(client.stream().closeReceived());
    EXPECT_EQ(1001U, client.stream().closeCode());
}

/// A listener whose port is already taken by a WebSocket listener.
TEST_F(XMQ_WebSocketTests, WebSocket_ListenerProtocolNames)
{
    using sptk::ServerConnection;

    EXPECT_EQ(ServerConnection::Type::TCP, connectionTypeForProtocol("MQTT"));
    EXPECT_EQ(ServerConnection::Type::SSL, connectionTypeForProtocol("MQTT+SSL"));
    EXPECT_EQ(ServerConnection::Type::WS, connectionTypeForProtocol("MQTT+WS"));
    EXPECT_EQ(ServerConnection::Type::WSS, connectionTypeForProtocol("mqtt+wss"));

    EXPECT_TRUE(ServerConnection::isEncrypted(ServerConnection::Type::WSS));
    EXPECT_FALSE(ServerConnection::isEncrypted(ServerConnection::Type::WS));

    EXPECT_THROW(connectionTypeForProtocol("MQTT+TCP"), sptk::Exception);
}
