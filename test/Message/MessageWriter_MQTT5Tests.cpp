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

#include "common/GenericProtocols.h"
#include "common/SubscribeAckMessage.h"
#include <base/AckMessage.h>
#include <common/ConnectMessage.h>
#include <common/mqtt/MessageReader.h>
#include <common/mqtt/MessageWriter.h>
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_Message, MessageWriter_MQTT5)
{
    Buffer           messageBuffer;
    auto             topicManager = make_shared<TopicManager>();
    GenericProtocols genericProtocols(topicManager);

    const auto& mqtt5 = genericProtocols.getProtocol(ProtocolVersion::MqttV5);
    const auto& messageWriter = mqtt5.messageWriter();

    const vector messageTypes {
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
    };

    for (auto messageType: messageTypes)
    {
        constexpr uint16_t testMessageId = 12345;
        switch (messageType)
        {
            using enum Message::Type;
            case Connect: {
                constexpr uint16_t keepAliveSeconds = 30;
                ConnectCredentials credentials {"client-1", "user", "secret"};
                const auto         lastWill = make_shared<LastWillInfo>("last/will/topic", "Last will message", false);
                ConnectMessage     connectMessage(credentials, lastWill, true, ProtocolVersion::MqttV31, keepAliveSeconds);
                messageWriter->writeConnect(messageBuffer, &connectMessage, nullptr);
            }
            break;

            case ConnectAck: {
                auto connectProperties = make_shared<MessageProperties>();
                messageWriter->appendConnectAckToBuffer(messageBuffer, connectProperties, ReasonCode::Success, false);
                break;
            }

            case Publish: {
                constexpr uint16_t packetId = 1;
                auto               message = make_shared<mqtt::PublishMessage>(topicManager->getTopic("devices/usb/1"), string_view("This is a test message"), packetId, false);
                message->setQos(Qos::Qos1);
                MessageFlags flags(Qos::Qos1, false, false);
                messageWriter->appendPublishToBuffer(messageBuffer, *message, flags, packetId, 0, {}, 1024);
            }
            break;

            case PublishAck: {
                auto message = make_shared<mqtt::PublishMessage>(topicManager->getTopic("devices/usb/1"), string_view("This is a test message"), testMessageId, false);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
            }
            break;

            case PublishReceived: {
                auto message = make_shared<mqtt::PublishMessage>(topicManager->getTopic("devices/usb/1"), string_view("This is a test message"), testMessageId, false);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
            }
            break;

            case PublishRelease: {
                auto message = make_shared<AckMessage>(PublishReceived, testMessageId);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
            }
            break;

            case PublishComplete: {
                auto message = make_shared<AckMessage>(PublishRelease, testMessageId);
                message->setQos(Qos::Qos2);
                messageWriter->appendAckToBuffer(messageBuffer, message.get(), ReasonCode::Success);
            }
            break;

            case Subscribe: {
                Destinations destinations;
                destinations.emplace_back(topicManager->getTopic("devices/usb"), SubscriptionOptions(Qos::Qos1));
                destinations.emplace_back(topicManager->getTopic("devices/eth"), SubscriptionOptions(Qos::Qos2));
                auto subscribeMessage = make_shared<SubscribeMessage>(destinations);
                subscribeMessage->setId(testMessageId);
                subscribeMessage->setQos(Qos::Qos1);
                messageWriter->appendSubscribeToBuffer(messageBuffer, *subscribeMessage, testMessageId);
            }
            break;

            case SubscribeAck: {
                const std::vector<uint8_t> grantedQosVector {0, 1, 2};
                SubscribeAckMessage        subscribeAckMessage(testMessageId, grantedQosVector);
                messageWriter->appendMessageToBuffer(messageBuffer, subscribeAckMessage, MessageFlags(), testMessageId, 0, {}, 1024);
            }
            break;

            case Unsubscribe: {
                Destinations destinations;
                destinations.emplace_back(topicManager->getTopic("devices/usb"));
                destinations.emplace_back(topicManager->getTopic("devices/eth"));
                auto unsubscribeMessage = make_shared<UnsubscribeMessage>(destinations);
                unsubscribeMessage->setId(testMessageId);
                unsubscribeMessage->setQos(Qos::Qos1);
                messageWriter->appendUnsubscribeToBuffer(messageBuffer, *unsubscribeMessage, testMessageId);
            }
            break;

            case UnsubscribeAck: {
                Destinations destinations({Destination(topicManager->getTopic("devices/usb"), SubscriptionOptions(Qos::Qos1))});
                auto         unsubscribeMessage = make_shared<UnsubscribeMessage>(destinations);
                unsubscribeMessage->setId(testMessageId);
                messageWriter->appendAckToBuffer(messageBuffer, unsubscribeMessage.get(), ReasonCode::Success);
            }
            break;

            case PingReq:
                messageWriter->appendPingRequestToBuffer(messageBuffer);
                break;

            case PingResp:
                messageWriter->appendPingResponseToBuffer(messageBuffer);
                break;

            case Disconnect:
                messageWriter->appendDisconnectToBuffer(messageBuffer, ReasonCode::Success);
                break;

            case Undefined:
                FAIL() << "Not supported reply for message type " << Message::messageTypeName(messageType);
        }
    }

    const auto& packetReader = mqtt5.packetReader();

    auto   done = false;
    size_t offset = 0;
    size_t messageTypeIndex = 0;
    while (!done)
    {
        bool empty = true;
        auto packet = packetReader->readPacket(messageBuffer, empty, offset);
        if (empty)
        {
            done = true;
        }
        else
        {
            auto frameType = static_cast<mqtt::FrameTypeTests>(packet.header()[0] & 0xf0);
            auto messageType = mqtt::frameTypeToMessageType(frameType);
            EXPECT_EQ(messageTypes[messageTypeIndex], messageType);
            ++messageTypeIndex;
        }
    }
}
