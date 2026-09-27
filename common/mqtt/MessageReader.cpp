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

#include "MessageReader.h"
#include "base/ProtocolException.h"
#include "common/ConnectAckMessage.h"
#include "common/ConnectMessage.h"
#include "common/DisconnectMessage.h"
#include "common/SubscribeAckMessage.h"
#include "common/SubscribeMessage.h"
#include "common/SubscriptionOptions.h"
#include "common/UnsubscribeMessage.h"
#include "common/mqtt/PublishMessage.h"
#include "base/MessageProperties.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace xmq::mqtt {

MessageReader::MessageReader(const STopicManager& topicManager, const ProtocolVersion protocolVersion)
    : xmq::MessageReader(protocolVersion)
    , m_topicManager(topicManager)
{
}

SMessage MessageReader::readMessage(Packet&& packet, BaseClientSession& clientSession, uint64_t packetReceivedTS) const
{
    const auto& messageHeader = packet.header<FixedHeader>();
    const auto  frameType = static_cast<FrameTypeTests>(messageHeader.m_type << 4U);
    const auto  remainingLength = static_cast<uint32_t>(packet.bytes());

    SMessage message;

    if (remainingLength > clientSession.getMaximumPacketSize())
    {
        return message;
    }

    switch (frameType)
    {
        using enum FrameTypeTests;

        case Publish:
            return readPublish(std::move(packet), messageHeader, clientSession, packetReceivedTS);

        case PubAck:
        case PubComp:
        case PubRec:
        case PubRel:
        case UnsubAck:
            return readGenericAck(packet, frameType);

        case Subscribe:
            message = readSubscribe(packet, remainingLength);
            break;

        case SubAck:
            message = readSubscribeAck(packet);
            break;

        case Unsubscribe:
            message = readUnsubscribe(packet, remainingLength);
            break;

        case PingReq:
        case PingResp:
            message = readNoPayloadMessage(frameType);
            break;

        case Connect:
            return readConnect(packet, clientSession, packetReceivedTS);

        case ConnAck:
            return readConnectAck(packet);

        case Disconnect:
            message = readDisconnect(packet);
            break;

        default:
            throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Not supported frame type: " + to_string(static_cast<int>(frameType)));
    }

    if (message && message->getQos() == Qos::Invalid)
    {
        throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Invalid QoS");
    }

    message->setSender(clientSession.getClientIdUnlocked());

    return message;
}

SMessage MessageReader::readConnect(Packet& packet, BaseClientSession& clientSession, uint64_t packetReceivedTS)
{
    const auto beforeDecodeTS = LatencyTrace::now();
    const auto protocolName = packet.readString();
    const auto protocolVersionByte = packet.readByte();

    auto reasonCode = ReasonCode::Success;
    if (protocolName != "MQIsdp" && protocolName != "MQTT")
    {
        reasonCode = protocolVersionByte == static_cast<uint8_t>(ProtocolVersion::MqttV5)
                         ? ReasonCode::UnsupportedProtocolVersion
                         : ReasonCode::ErrorInvalidProtocol;
    }

    if (protocolVersionByte < static_cast<uint8_t>(ProtocolVersion::MqttV31) || protocolVersionByte > static_cast<uint8_t>(ProtocolVersion::MqttV5))
    {
        reasonCode = ReasonCode::ErrorInvalidProtocol;
    }

    auto connectMessage = make_shared<ConnectMessage>();

    if (reasonCode == ReasonCode::Success)
    {
        const auto connectMessageParameters = connectMessage->getParameters();
        connectMessageParameters->m_protocolVersion = static_cast<ProtocolVersion>(protocolVersionByte);

        auto       connectFlagsByte = packet.readByte();
        const auto connectFlags = *bit_cast<ConnectFlags*>(&connectFlagsByte);

        connectMessageParameters->m_keepAliveSec = packet.readShortInteger();

        const auto expectUsername = connectFlags.username;
        const auto expectPassword = connectFlags.password;

        connectMessageParameters->m_cleanSession = connectFlags.cleanSession;
        const auto hasLastWill = connectFlags.willFlag;

        if (protocolVersionByte == static_cast<uint8_t>(ProtocolVersion::MqttV5))
        {
            if (auto [properties, size] = readPropertiesMqtt5(packet);
                properties)
            {
                clientSession.applyConnectProperties(*properties, reasonCode);
                connectMessage->setProperties(properties);
            }
        }

        // Variable payload
        connectMessageParameters->setClientId(packet.readString());

        if (hasLastWill)
        {
            connectMessageParameters->m_lastWill = make_shared<LastWillInfo>();
            reasonCode = readLastWillInfo(packet, protocolVersionByte, connectMessageParameters->m_lastWill, connectFlags);
        }

        if (expectUsername)
        {
            connectMessage->setUsername(packet.readString());
            if (expectPassword)
            {
                connectMessage->setPassword(packet.readString());
            }
        }

        if (connectFlags.reserved)
        {
            reasonCode = protocolVersionByte == static_cast<uint8_t>(ProtocolVersion::MqttV5) ? ReasonCode::MalformedPacket : ReasonCode::ErrorInvalidProtocol;
        }
    }

    connectMessage->setReasonCode(reasonCode);

    if (const auto messageProperties = connectMessage->getProperties())
    {
        Latency::SET_LATENCY(messageProperties, LatencyPhase::ServerWireIn, packetReceivedTS);
        Latency::SET_LATENCY(messageProperties, LatencyPhase::ServerBeforeDecode, beforeDecodeTS);
        Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ServerDecode);
    }

    return connectMessage;
}

ReasonCode MessageReader::readLastWillInfo(Packet& packet, const uint8_t protocolVersionByte, const SLastWillInfo& lastWill, const ConnectFlags connectFlags)
{
    auto reasonCode {ReasonCode::Success};

    if (protocolVersionByte >= static_cast<uint8_t>(ProtocolVersion::MqttV5))
    {
        uint32_t readSize {0};
        tie(lastWill->m_properties, readSize) = readPropertiesMqtt5(packet);
    }
    lastWill->m_topic = packet.readString();
    lastWill->m_message = packet.readString();
    lastWill->m_qos = connectFlags.willQos;
    lastWill->m_retain = connectFlags.willRetain;

    if (lastWill->m_qos == Qos::Invalid)
    {
        reasonCode = ReasonCode::MalformedPacket;
    }

    return reasonCode;
}

SMessage MessageReader::readPublish(Packet&& packet, const FixedHeader& messageHeader, BaseClientSession& clientSession, uint64_t packetReceivedTS) const
{
    const auto beforeDecodeTS = LatencyTrace::now();
    if (messageHeader.m_qos == Qos::Qos0 && messageHeader.m_dup != 0)
    {
        throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Messages with QOS0 can't have isDuplicate flag set");
    }

    if (messageHeader.m_qos > Qos::Qos2)
    {
        throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Invalid QOS");
    }

    SMessage publishMessage = make_shared<PublishMessage>(m_topicManager, messageHeader, std::move(packet), getProtocolVersion());

    if (publishMessage->getProperties())
    {
        if (const auto reasonCode = publishMessage->getProperties()->validate();
            reasonCode != ReasonCode::Success)
        {
            throw ProtocolException(getProtocolVersion(), reasonCode, toString(reasonCode));
        }

        if (int64_t topicAlias = 0;
            publishMessage->getProperties()->takeProperty(Property::TopicAlias, topicAlias))
        {
            clientSession.setPublishDestinationFromTopicAlias(static_cast<PublishMessage*>(publishMessage.get()), topicAlias);
        }
    }

    if (const auto messageProperties = publishMessage->getProperties())
    {
        if (clientSession.getSessionType() == BaseClientSession::SessionType::Client)
        {
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ClientWireIn, packetReceivedTS);
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ClientBeforeDecode, beforeDecodeTS);
            Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ClientDecode);
        }
        else
        {
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ServerWireIn, packetReceivedTS);
            Latency::SET_LATENCY(messageProperties, LatencyPhase::ServerBeforeDecode, beforeDecodeTS);
            Latency::SNAP_LATENCY(messageProperties, LatencyPhase::ServerDecode);
        }
    }

    return publishMessage;
}

SMessage MessageReader::readConnectAck(Packet& packet) const
{
    auto remainingLength = packet.bytes();

    constexpr auto headerLength = 2;
    const auto     connectAck = packet.readByte();
    auto           connectRc = packet.readByte();
    remainingLength -= headerLength;

    auto connectAckMessage = make_shared<ConnectAckMessage>(static_cast<ReasonCode>(connectRc), connectAck & 1U);

    if (remainingLength > 0)
    {
        readProperties(packet, connectAckMessage.get());
    }

    return connectAckMessage;
}

SMessage MessageReader::readGenericAck(Packet& packet, const FrameTypeTests frameType)
{
    if (packet.bytes() == 0)
    {
        return nullptr;
    }

    const auto messageId = packet.readId();
    const auto reasonCode = packet.bytes() > sizeof(uint16_t)
                                ? static_cast<ReasonCode>(packet.readByte())
                                : ReasonCode::Success;

    auto ackMessage = make_shared<AckMessage>(frameTypeToMessageType(frameType), messageId);
    ackMessage->setReasonCode(reasonCode);

    return ackMessage;
}

SMessage MessageReader::readSubscribe(Packet& packet, const uint32_t remainingLength) const
{
    constexpr auto variableHeaderLength = 2;

    const auto messageId = packet.readId();

    auto subscriptionMessage = make_shared<SubscribeMessage>();
    subscriptionMessage->setId(messageId);

    const auto propertiesLength = readProperties(packet, subscriptionMessage.get());

    auto dataLength = remainingLength - variableHeaderLength - static_cast<uint32_t>(propertiesLength);

    Destinations topics;

    while (dataLength > 1)
    {
        auto topicStr = packet.readString();
        if (topicStr.empty())
        {
            throw ProtocolException(getProtocolVersion(), ReasonCode::ProtocolError, "Topic is empty in Subscribe message");
        }

        // Subscription options (one byte)
        SubscriptionOptions subscribeOptions;
        packet.readData(subscribeOptions);
        if (static_cast<Qos>(subscribeOptions.m_maxQos) == Qos::Invalid)
        {
            throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Invalid maximum QoS in Subscribe message");
        }
        if (subscribeOptions.m_reserved != 0)
        {
            throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Invalid reserved bits Subscribe message");
        }
        if (getProtocolVersion() != ProtocolVersion::MqttV5)
        {
            subscribeOptions.m_retainAsPublished = false;
            subscribeOptions.m_retainHandling = 0;
        }

        constexpr auto topicSizeAndOptions = 3;
        const auto*    topic = m_topicManager->getTopic(topicStr);
        topics.emplace_back(topic, subscribeOptions);
        dataLength -= static_cast<uint32_t>(topicStr.size() + topicSizeAndOptions);
    }

    subscriptionMessage->setDestinations(std::move(topics));

    return subscriptionMessage;
}

SMessage MessageReader::readUnsubscribe(Packet& packet, const uint32_t remainingLength) const
{
    constexpr auto variableHeaderLength = 2;

    const auto messageId = packet.readId();

    auto unsubscribeMessage = make_shared<UnsubscribeMessage>();
    unsubscribeMessage->setId(messageId);

    const auto propertiesLength = static_cast<uint32_t>(readProperties(packet, unsubscribeMessage.get()));

    auto dataLength = remainingLength - variableHeaderLength - propertiesLength;

    Destinations topics;

    while (dataLength > 1)
    {
        auto        topicStr = packet.readString();
        const auto* topic = m_topicManager->getTopic(topicStr);
        topics.emplace_back(topic);
        dataLength -= static_cast<uint32_t>(topicStr.size() + sizeof(uint16_t));
    }

    unsubscribeMessage->setDestinations(std::move(topics));

    return unsubscribeMessage;
}

SMessage MessageReader::readDisconnect(Packet& packet) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        const auto remainingLength = packet.bytes();

        ReasonCode reasonCode = remainingLength > 0 ? static_cast<ReasonCode>(packet.readByte()) : ReasonCode::Success;

        auto message = make_shared<DisconnectMessage>(reasonCode);

        if (remainingLength > 1)
        {
            readProperties(packet, message.get());
        }

        return message;
    }
    return make_shared<DisconnectMessage>(ReasonCode::Success);
}

SMessage MessageReader::readSubscribeAck(Packet& packet) const
{
    const auto remainingLength = packet.bytes();

    auto variableHeaderLength = 0;

    constexpr auto messageIdLength = 2;
    const auto     messageId = packet.readId();
    variableHeaderLength += messageIdLength;

    auto dataLength = remainingLength - variableHeaderLength;

    auto subscribeAckMessage = make_shared<SubscribeAckMessage>(messageId);

    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        const auto propertiesLength = readProperties(packet, subscribeAckMessage.get());
        dataLength -= propertiesLength;
    }

    vector<uint8_t>& subscriptionResults = subscribeAckMessage->subscriptionResults();
    subscriptionResults.resize(dataLength);
    packet.readData(subscriptionResults.data(), dataLength);

    for (const auto& qos: subscriptionResults)
    {
        if (qos > static_cast<uint8_t>(Qos::Invalid))
        {
            subscribeAckMessage->setReasonCode(static_cast<ReasonCode>(qos));
        }
    }
    return subscribeAckMessage;
}

SMessage MessageReader::readNoPayloadMessage(const FrameTypeTests frameType)
{
    auto messageType = frameTypeToMessageType(frameType);
    return make_shared<AckMessage>(messageType, static_cast<MessageId>(0));
}

std::tuple<SMessageProperties, uint32_t> MessageReader::readPropertiesMqtt5(Packet& packet)
{
    shared_ptr<MessageProperties> properties;

    const auto propertiesLength = packet.readVariableLength(1);
    const auto variableLengthSize = VariableLength::bytes(propertiesLength);

    if (propertiesLength != 0)
    {
        Buffer propertiesData(propertiesLength);
        packet.readData(propertiesData.data(), propertiesLength);
        propertiesData.bytes(propertiesLength);

        properties = make_shared<MessageProperties>();
        properties->read(propertiesData.data(), propertiesData.size());
    }

    return {properties, variableLengthSize + propertiesLength};
}

size_t MessageReader::readProperties(Packet& packet, Message* message) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        auto [properties, size] = readPropertiesMqtt5(packet);
        message->setProperties(properties);
        return size;
    }
    return 0;
}

} // namespace xmq::mqtt
