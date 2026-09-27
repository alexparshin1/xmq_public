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

#include "MessageReaderTests.h"
#include "base/AckMessage.h"
#include "base/ReasonCode.h"
#include "client/Session.h"
#include "common/SubscribeAckMessage.h"
#include "common/SubscribeMessage.h"
#include "common/UnsubscribeMessage.h"
#include "common/mqtt/MessageWriter.h"
#include "common/mqtt/PublishMessage.h"
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

const uint16_t XMQ_MessageReaderTests::m_portNumber = 3000;

class MessageReaderServer : public FastTCPServer
{
public:
    explicit MessageReaderServer(const std::function<void(const shared_ptr<ServerConnection>& connection)>& connectionFunction)
        : FastTCPServer("")
        , m_connectionFunction(connectionFunction)
    {
    }

    void socketEventCallback(const shared_ptr<ServerConnection>& connection, const SocketEventType) override
    {
        unwatchConnection(connection);
        m_connectionFunction(connection);
    }

private:
    std::function<void(const shared_ptr<ServerConnection>& connection)> m_connectionFunction;
};

void XMQ_MessageReaderTests::SetUp()
{
    createServer();
}

void XMQ_MessageReaderTests::createServer()
{
    m_server = make_shared<MessageReaderServer>([](const shared_ptr<ServerConnection>& serverConnection)
                                                {
                                                    messageReaderTestFunction(*serverConnection);
                                                });

    m_server->addListener(ServerConnection::Type::TCP, {"0.0.0.0", m_portNumber});

    m_clientSocket = make_shared<TCPSocket>();
}

void XMQ_MessageReaderTests::messageReaderTestFunction(const ServerConnection& serverConnection)
{
    auto   messageType = Message::Type::Publish;
    size_t messageCount {0};

    MessageWriters messageWriters;

    auto messageWriter = messageWriters.create(ProtocolVersion::MqttV311);

    auto socket = serverConnection.getSocket();

    // The connection has been unwatched, so the socket pool no longer drives it. The test helper
    // drives the dialog synchronously and relies on reads blocking until the next request arrives,
    // so switch the (pool-default non-blocking) socket back to blocking mode.
    socket->blockingMode(true);

    while (messageType != Message::Type::Disconnect)
    {
        if (!socket->active())
        {
            break;
        }

        uint8_t marker {0};
        while (true)
        {
            socket->read(marker);
            if (marker == m_commandStartMarker)
            {
                break;
            }
        }
        socket->read(messageType);
        socket->read(messageCount);

        constexpr MessageId testMessageId {1234};
        Buffer              messageBuffer;

        switch (messageType)
        {
            using enum Message::Type;
            case Connect: {
                constexpr uint16_t keepAliveSeconds = 30;
                ConnectCredentials credentials {"client-1", "user", "secret"};
                auto               lastWill = make_shared<LastWillInfo>("last/will/topic", "Last will message", false);
                ConnectMessage     connectMessage(credentials, lastWill, true, ProtocolVersion::MqttV31, keepAliveSeconds);
                messageWriter->writeConnect(messageBuffer, &connectMessage, nullptr);
                socket->write(messageBuffer);
            }
            break;

            case ConnectAck: {
                auto connectProperties = make_shared<MessageProperties>();
                messageWriter->appendConnectAckToBuffer(messageBuffer, connectProperties, ReasonCode::Success, false);
                socket->write(messageBuffer);
                break;
            }

            case Publish: {
                constexpr uint16_t packetId = 1;
                auto               message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("devices/usb/1"),
                                                                               string_view("This is a test message"), packetId, false);
                message->setQos(Qos::Qos1);
                MessageFlags flags(Qos::Qos1, false, false);
                messageWriter->appendPublishToBuffer(messageBuffer, *message, flags, packetId, 0, {}, 1024);
                socket->write(messageBuffer);
            }
            break;

            case PublishAck: {
                auto message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("devices/usb/1"),
                                                                 string_view("This is a test message"), testMessageId, false);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
                socket->write(messageBuffer);
            }
            break;

            case PublishReceived: {
                auto message = make_shared<mqtt::PublishMessage>(client::MqttClient::getTopic("devices/usb/1"),
                                                                 string_view("This is a test message"), testMessageId, false);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
                socket->write(messageBuffer);
            }
            break;

            case PublishRelease: {
                auto message = make_shared<AckMessage>(PublishReceived, testMessageId);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
                socket->write(messageBuffer);
            }
            break;

            case PublishComplete: {
                auto message = make_shared<AckMessage>(PublishRelease, testMessageId);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
                socket->write(messageBuffer);
            }
            break;

            case Subscribe: {
                using enum Qos;
                Destinations destinations;
                destinations.emplace_back(client::MqttClient::getTopic("devices/usb"), SubscriptionOptions(Qos1));
                destinations.emplace_back(client::MqttClient::getTopic("devices/eth"), SubscriptionOptions(Qos2));
                auto subscribeMessage = make_shared<SubscribeMessage>(destinations);
                subscribeMessage->setId(testMessageId);
                subscribeMessage->setQos(Qos1);
                messageWriter->appendSubscribeToBuffer(messageBuffer, *subscribeMessage, testMessageId);
                socket->write(messageBuffer);
            }
            break;

            case SubscribeAck: {
                const std::vector<uint8_t> grantedQosVector {0, 1, 2};
                SubscribeAckMessage        subscribeAckMessage(testMessageId, grantedQosVector);
                Buffer                     buffer;
                messageWriter->appendMessageToBuffer(buffer, subscribeAckMessage, MessageFlags(), testMessageId, 0, {}, 1024);
                socket->write(buffer);
            }
            break;

            case Unsubscribe: {
                Destinations destinations;
                destinations.emplace_back(client::MqttClient::getTopic("devices/usb"));
                destinations.emplace_back(client::MqttClient::getTopic("devices/eth"));
                auto unsubscribeMessage = make_shared<UnsubscribeMessage>(destinations);
                unsubscribeMessage->setId(testMessageId);
                unsubscribeMessage->setQos(Qos::Qos1);
                messageWriter->appendUnsubscribeToBuffer(messageBuffer, *unsubscribeMessage, testMessageId);
                socket->write(messageBuffer);
            }
            break;

            case UnsubscribeAck: {
                Destinations destinations({Destination(client::MqttClient::getTopic("devices/usb"), SubscriptionOptions(Qos::Qos1))});
                auto         unsubscribeMessage = make_shared<UnsubscribeMessage>(destinations);
                unsubscribeMessage->setId(testMessageId);
                messageWriter->appendAckToBuffer(messageBuffer, unsubscribeMessage.get(), ReasonCode::Success);
                socket->write(messageBuffer);
            }
            break;

            case PingReq:
                messageWriter->appendPingRequestToBuffer(messageBuffer);
                socket->write(messageBuffer);
                break;

            case PingResp:
                messageWriter->appendPingResponseToBuffer(messageBuffer);
                socket->write(messageBuffer);
                break;

            case Disconnect:
                messageWriter->appendDisconnectToBuffer(messageBuffer, ReasonCode::Success);
                socket->write(messageBuffer);
                break;

            case Undefined:
                FAIL() << "Not supported reply for message type " << Message::messageTypeName(messageType);
        }
    }

    this_thread::sleep_for(1s);
    socket->close();
}

shared_ptr<TCPSocket> XMQ_MessageReaderTests::clientSocket() const
{
    return m_clientSocket;
}

namespace {
void verifyConnectMessage(Message* message)
{
    auto* connectMessage = dynamic_cast<ConnectMessage*>(message);

    EXPECT_TRUE(connectMessage);

    EXPECT_STREQ("client-1", connectMessage->getClientId().c_str());
    EXPECT_STREQ("user", connectMessage->getUsername().c_str());
    EXPECT_STREQ("secret", connectMessage->getPassword().c_str());
    EXPECT_TRUE(connectMessage->getParameters()->m_lastWill);
    EXPECT_STREQ("last/will/topic", connectMessage->getParameters()->m_lastWill->m_topic.c_str());
    EXPECT_STREQ("Last will message", connectMessage->getParameters()->m_lastWill->m_message.c_str());
}

void verifyConnectAckMessage(const Message* message)
{
    const auto* ackMessage = dynamic_cast<const AckMessage*>(message);

    EXPECT_TRUE(ackMessage);

    EXPECT_EQ(0, ackMessage->getId());
}

void verifyGenericAckMessage(const Message* message)
{
    const auto* ackMessage = dynamic_cast<const AckMessage*>(message);

    EXPECT_TRUE(ackMessage);

    EXPECT_EQ(1234, ackMessage->getId());
}

void verifyPublishMessage(const Message* message)
{
    const auto* publishMessage = dynamic_cast<const PublishMessage*>(message);

    EXPECT_TRUE(publishMessage);

    EXPECT_STREQ("devices/usb/1", publishMessage->destination()->toString().data());
    const string messageData(bit_cast<const char*>(publishMessage->payloadData()), publishMessage->payloadSize());
    EXPECT_STREQ("This is a test message", messageData.c_str());
    EXPECT_EQ(Qos::Qos1, publishMessage->getQos());
}

void verifySubscribeMessage(const Message* message)
{
    const auto* subscribeMessage = dynamic_cast<const SubscribeMessage*>(message);

    EXPECT_TRUE(subscribeMessage);

    Strings destinations;
    for (const auto& destination: subscribeMessage->getDestinations())
    {
        destinations.push_back(destination.toString());
    }

    EXPECT_STREQ("devices/usb:qos1:RetainAlways;devices/eth:qos2:RetainAlways", destinations.join(";").c_str());
    EXPECT_EQ(Qos::Qos1, subscribeMessage->getQos());
}

void verifySubscribeAckMessage(const Message* message)
{
    const auto* ackMessage = dynamic_cast<const SubscribeAckMessage*>(message);

    EXPECT_TRUE(ackMessage);

    EXPECT_EQ(1234, ackMessage->getId());

    constexpr auto expectedQosVectorSize = 3U;
    ASSERT_EQ(expectedQosVectorSize, ackMessage->subscriptionResults().size());
    EXPECT_EQ(0, ackMessage->subscriptionResults()[0]);
    EXPECT_EQ(1, ackMessage->subscriptionResults()[1]);
    EXPECT_EQ(2, ackMessage->subscriptionResults()[2]);
}

void verifyUnsubscribeMessage(const Message* message)
{
    const auto* unsubscribeMessage = dynamic_cast<const UnsubscribeMessage*>(message);

    EXPECT_TRUE(unsubscribeMessage);

    Strings destinations;
    for (const auto& destination: unsubscribeMessage->destinations())
    {
        destinations.push_back(destination.m_topic->toString().data());
    }

    EXPECT_STREQ("devices/usb;devices/eth", destinations.join(";").c_str());
}
} // namespace

TEST_F(XMQ_MessageReaderTests, readAllMessageTypes_Socket)
{
    constexpr uint16_t testServerPort = 3000;
    clientSocket()->open(Host("localhost", testServerPort));
    ASSERT_TRUE(clientSocket()->active());

    auto       topicManager = make_shared<TopicManager>();
    const auto messageReaders = make_shared<MessageReaders>(topicManager);

    try
    {
        const auto packetReader = PacketReader::factory(ProtocolVersion::MqttV31);
        const auto clientReader = messageReaders->create(ProtocolVersion::MqttV31);

        const vector messageTypes = {
            Message::Type::Connect,
            Message::Type::ConnectAck,
            Message::Type::Publish,
            Message::Type::PublishAck,
            Message::Type::PublishReceived,
            Message::Type::PublishRelease,
            Message::Type::PublishComplete,
            Message::Type::Subscribe,
            Message::Type::SubscribeAck,
            Message::Type::Unsubscribe,
            Message::Type::UnsubscribeAck,
            Message::Type::PingReq,
            Message::Type::PingResp,
            Message::Type::Disconnect,
        };

        auto           messageCount = static_cast<size_t>(1);
        constexpr auto marker = m_commandStartMarker;

        client::Session session(nullptr, nullptr);

        for (auto messageType: messageTypes)
        {
            // Send marker before every command so that auto-ACKs can be ignored on the server side
            clientSocket()->write(&marker, sizeof(marker));

            clientSocket()->write(bit_cast<uint8_t*>(&messageType), sizeof(messageType));
            clientSocket()->write(bit_cast<uint8_t*>(&messageCount), sizeof(messageCount));

            ASSERT_TRUE(clientSocket()->readyToRead(1s));

            const auto packetReceivedTS = LatencyTrace::now();
            auto       packet = packetReader->readPacket(clientSocket().get());
            if (packet.bytes() == 0)
            {
                continue;
            }


            auto aMessage = clientReader->readMessage(std::move(packet), session, packetReceivedTS);
            if (!aMessage)
            {
                continue;
            }

            auto* message = aMessage.get();

            COUT(message->toString());

            switch (message->type())
            {
                using enum Message::Type;
                case Connect:
                    verifyConnectMessage(message);
                    break;
                case ConnectAck:
                    verifyConnectAckMessage(message);
                    break;
                case Publish:
                    verifyPublishMessage(message);
                    break;
                case PublishAck:
                case PublishReceived:
                case PublishRelease:
                case PublishComplete:
                case UnsubscribeAck:
                    verifyGenericAckMessage(message);
                    break;
                case Subscribe:
                    verifySubscribeMessage(message);
                    break;
                case SubscribeAck:
                    verifySubscribeAckMessage(message);
                    break;
                case Unsubscribe:
                    verifyUnsubscribeMessage(message);
                    break;
                case PingReq:
                case PingResp:
                case Disconnect:
                    // Nothing to verify: the message has no id or data
                    break;
                case Undefined:
                    FAIL() << "Unhandled message type " << Message::messageTypeName(message->type());
            }
        }

        this_thread::sleep_for(m_tinyTimeout);
    }
    catch (const Exception& e)
    {
        FAIL() << e.what();
    }
}
