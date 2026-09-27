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

#include "MessageWriter.h"

#include "base/ProtocolException.h"
#include "common/DisconnectMessage.h"
#include "common/PublishMessage.h"
#include "common/SubscribeAckMessage.h"
#include "common/SubscribeMessage.h"
#include "common/SubscriptionOptions.h"
#include "common/UnsubscribeMessage.h"

using namespace std;
using namespace sptk;

namespace xmq::mqtt {

void MessageWriter::appendVariableHeader(uint8_t*& tail, const char* data, const uint16_t dataLength)
{
    appendShortValue(tail, dataLength);
    if (dataLength != 0)
    {
        memcpy(tail, data, dataLength);
        tail += dataLength;
    }
}

void MessageWriter::appendVariableLength(uint8_t*& tail, const uint32_t variableLengthValue)
{
    auto lengthValue = variableLengthValue;
    do
    {
        constexpr uint8_t maxValueInByte = 128;
        auto              digit = static_cast<uint8_t>(lengthValue % maxValueInByte);
        lengthValue /= maxValueInByte;
        // if there are m_more digits to encode, set the top bit of this digit
        if (lengthValue > 0)
        {
            digit |= maxValueInByte;
        }
        *tail = digit;
        ++tail;
    } while (lengthValue > 0);
}

void MessageWriter::writeConnect(Buffer& messageBuffer, const ConnectMessage* connectMessage, const TweakMessageCallback& tweakMessage) const
{
    string_view protocolName;
    uint32_t    headerLength {0};

    const auto            connectMessageParameters = connectMessage->getParameters();
    const auto&           protocolVersion = static_cast<ProtocolVersion>(connectMessageParameters->m_protocolVersion);
    constexpr auto        headerLengthMqtt31 = 12;
    constexpr auto        headerLengthMqtt311 = 10;
    constexpr string_view protocolNameV31("MQIsdp", 6);
    constexpr string_view protocolNameV5("MQTT", 4);
    switch (protocolVersion)
    {
        using enum ProtocolVersion;
        case MqttV31:
            headerLength = headerLengthMqtt31;
            protocolName = protocolNameV31;
            break;
        case MqttV311:
        case MqttV5:
            headerLength = headerLengthMqtt311;
            protocolName = protocolNameV5;
            break;
    }

    uint8_t     connectFlags = 0;
    const auto& clientId = connectMessageParameters->getClientId();

    unsigned expectedPropertiesSize {0};
    unsigned extraPacketSize {0};
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        computePropertiesMetrics(connectMessage->getProperties(), {}, expectedPropertiesSize, extraPacketSize);
    }

    auto payloadLength = static_cast<uint32_t>(clientId.length()) + 2 + extraPacketSize;

    if (connectMessageParameters->m_cleanSession != 0)
    {
        connectFlags |= static_cast<int>(ConnectFrameFlags::CleanSession);
    }

    constexpr auto stringLengthSize {2};

    // If username and password are defined - add their size to payload length
    if (!connectMessage->getUsername().empty())
    {
        connectFlags |= static_cast<int>(ConnectFrameFlags::Username);
        payloadLength += static_cast<int>(connectMessage->getUsername().length()) + stringLengthSize;
        if (!connectMessage->getPassword().empty())
        {
            connectFlags |= static_cast<int>(ConnectFrameFlags::Password);
            payloadLength += static_cast<uint32_t>(connectMessage->getPassword().length() + stringLengthSize);
        }
    }

    string_view willMessage("", 0);
    string_view willTopic("", 0);
    uint32_t    expectedWillPropertiesSize {0};

    // If the will topic and message are defined, add their size to payload length.
    const auto lastWill = connectMessageParameters->m_lastWill;
    if (lastWill)
    {
        willMessage = lastWill->m_message;
        willTopic = lastWill->m_topic;

        connectFlags |= static_cast<int>(ConnectFrameFlags::WillFlag);

        if (lastWill->m_qos == Qos::Qos1)
        {
            connectFlags |= static_cast<int>(ConnectFrameFlags::WillQos1);
        }

        if (lastWill->m_qos == Qos::Qos2)
        {
            connectFlags |= static_cast<int>(ConnectFrameFlags::WillQos2);
        }

        if (lastWill->m_retain)
        {
            connectFlags |= static_cast<int>(ConnectFrameFlags::WillRetain);
        }

        if (getProtocolVersion() == ProtocolVersion::MqttV5)
        {
            uint32_t extraWillPacketSize {0};
            computePropertiesMetrics(lastWill->m_properties, {}, expectedWillPropertiesSize, extraWillPacketSize);
            payloadLength += extraWillPacketSize;
        }
        payloadLength += static_cast<uint32_t>(willTopic.length() + stringLengthSize);
        payloadLength += static_cast<uint32_t>(willMessage.length() + stringLengthSize);
    }

    constexpr auto extraLength {4};
    messageBuffer.bytes(0);
    messageBuffer.reserve(headerLength + payloadLength + extraLength);
    auto* tail = messageBuffer.data();

    // Header
    appendByte(tail, static_cast<uint8_t>(FrameTypeTests::Connect));
    appendVariableLength(tail, headerLength + payloadLength); // Remaining Length
    appendString(tail, protocolName);                         // Protocol Name
    appendByte(tail, static_cast<uint8_t>(protocolVersion));  // Protocol Version

    appendByte(tail, connectFlags); // Connect Flags
    appendShortValue(tail, connectMessageParameters->m_keepAliveSec);

    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        // Connect message properties
        appendProperties(tail, connectMessage->getProperties(), expectedPropertiesSize, 0, {});
    }

    // Payload
    if (clientId.empty())
    {
        throw Exception("Client Id shouldn't be empty");
    }
    appendString(tail, clientId); // GenericClient id

    if (lastWill)
    {
        // Last will properties
        if (getProtocolVersion() == ProtocolVersion::MqttV5)
        {
            appendProperties(tail, lastWill->m_properties, expectedWillPropertiesSize, 0, {});
        }
        appendString(tail, willTopic);
        appendString(tail, willMessage);
    }

    if (!connectMessage->getUsername().empty())
    {
        appendString(tail, connectMessage->getUsername());
        if (!connectMessage->getPassword().empty())
        {
            appendString(tail, connectMessage->getPassword());
        }
    }

    messageBuffer.bytes(tail - messageBuffer.data());

    if (tweakMessage)
    {
        tweakMessage(messageBuffer);
    }
}

void MessageWriter::appendDisconnectToBuffer(Buffer& messageBuffer, ReasonCode reasonCode) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        constexpr auto fixedHeader = static_cast<uint8_t>(FrameTypeTests::Disconnect);
        messageBuffer.append(fixedHeader);

        // Two bytes follow, and the remaining length has to say so: the reason code and the
        // property length. Declaring 1 leaves the property length outside the frame, so the
        // peer stops reading one byte short and every packet after this one on the same
        // connection is parsed at the wrong offset.
        constexpr uint8_t remainingLength = 2;
        messageBuffer.append(remainingLength);
        messageBuffer.append(static_cast<uint8_t>(reasonCode));

        // The total properties length is 0
        messageBuffer.append(static_cast<uint8_t>(0));
    }
    else
    {
        appendAckToBuffer(messageBuffer, static_cast<int>(FrameTypeTests::Disconnect), 0, ReasonCode::Success);
    }
}

void MessageWriter::appendAckToBuffer(Buffer& messageBuffer, const Message* message, const ReasonCode reasonCode) const
{
    auto ackFrameType = messageTypeToAckFrameType(message->type(), message->getQos());
    appendAckToBuffer(messageBuffer, static_cast<int>(ackFrameType), message->getId(), reasonCode);
}

void MessageWriter::appendAckToBuffer(Buffer& messageBuffer, int frameType, const MessageId& ackMessageId, ReasonCode reasonCode) const
{
    const size_t extraLengthForMqtt5 = getProtocolVersion() == ProtocolVersion::MqttV5 ? 1 : 0;

    auto fixedHeader = static_cast<uint8_t>(frameType);
    if (ackMessageId > 0)
    {
        const auto dataOffset = messageBuffer.bytes();
        messageBuffer.reserve(dataOffset + 4 + extraLengthForMqtt5);
        messageBuffer.bytes(dataOffset + 4 + extraLengthForMqtt5);
        if (static_cast<FrameTypeTests>(frameType) == FrameTypeTests::PubRel)
        {
            constexpr auto qosBit {2};
            fixedHeader += qosBit; // For PubRel, getQos is 1
        }
        auto* tail = messageBuffer.data() + dataOffset;
        *tail++ = fixedHeader;

        // The remaining length
        *tail++ = static_cast<uint8_t>(2 + extraLengthForMqtt5);

        // Message id
        *bit_cast<uint16_t*>(tail) = htons(ackMessageId);
        tail += sizeof(uint16_t);

        *tail = static_cast<uint8_t>(reasonCode);

        return;
    }

    constexpr auto maxRemainingLengthSize {4};
    const auto     dataOffset = messageBuffer.bytes();
    messageBuffer.reserve(dataOffset + maxRemainingLengthSize);
    uint8_t* tail = messageBuffer.data() + dataOffset;

    // Fixed header
    appendByte(tail, fixedHeader);

    // The remaining length is zero for most ACKs
    *tail = static_cast<uint8_t>(0);
    ++tail;

    messageBuffer.bytes(tail - messageBuffer.data());
}

bool MessageWriter::appendPublishToBuffer(Buffer& buffer, const PublishMessage& message, const MessageFlags messageFlags, const MessageId& deliveryId,
                                          const uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds, const size_t maxPacketSize) const
{
    const SMessageProperties& messageProperties = message.getProperties();
    uint32_t                  expectedPropertiesSize = 0;
    uint32_t                  extraPacketSize = 0;
    computePropertiesMetrics(messageProperties, subscriptionIds, expectedPropertiesSize, extraPacketSize);

    constexpr auto dataLengthSize {2};
    const auto&    topic = message.destination();
    auto           packetLength = static_cast<uint32_t>(topic->size() + dataLengthSize + message.payloadSize()) + extraPacketSize;
    if (messageFlags.getQos() != Qos::Qos0)
    {
        // Need the extra two bytes for the message id
        packetLength += sizeof(MessageId);
    }

    constexpr auto maxRemainingLengthSize {4};
    const auto     dataOffset = buffer.bytes();
    const auto     expectedPacketSize {dataOffset + packetLength + maxRemainingLengthSize};

    if (expectedPacketSize > maxPacketSize)
    {
        return false;
    }

    buffer.reserve(expectedPacketSize); // Reserve space for up to four bytes of the remaining length
    uint8_t* tail = buffer.data() + dataOffset;

    constexpr auto frameTypeShift {4};

    // Fixed header
    FixedHeader header {};
    header.m_retain = messageFlags.isRetain();
    header.m_qos = messageFlags.getQos();
    header.m_type = static_cast<uint8_t>(FrameTypeTests::Publish) >> frameTypeShift;
    header.m_dup = messageFlags.isDuplicate();
    appendFixedHeader(tail, header);
    appendVariableLength(tail, packetLength);

    // Variable header
    appendVariableHeader(tail, topic->name().data(), static_cast<uint16_t>(topic->size()));
    if (messageFlags.getQos() != Qos::Qos0)
    {
        if (deliveryId == 0)
        {
            throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Message id is 0");
        }
        appendShortValue(tail, deliveryId);
    }

    // Properties
    appendProperties(tail, message.getProperties(), expectedPropertiesSize, remainingExpirationSeconds, subscriptionIds);

    // Payload
    appendData(tail, message.payloadData(), message.payloadSize());

    buffer.bytes(tail - buffer.data());

    return true;
}

void MessageWriter::appendSubscribeToBuffer(Buffer& buffer, const SubscribeMessage& message, const MessageId messageId) const
{
    uint32_t expectedPropertiesSize = 0;
    uint32_t extraPacketSize = 0;
    computePropertiesMetrics(message.getProperties(), {}, expectedPropertiesSize, extraPacketSize);

    const Destinations& destinations = message.getDestinations();

    uint32_t packetLength = 2 + extraPacketSize;
    for (const auto& destination: destinations)
    {
        constexpr auto topicLengthAndOptionSize {3};
        packetLength += static_cast<uint32_t>(destination.m_topic->size() + topicLengthAndOptionSize);
    }

    constexpr auto maxRemainingLengthSize {4};
    const auto     dataOffset = buffer.bytes();
    buffer.reserve(dataOffset + packetLength + maxRemainingLengthSize);
    uint8_t* tail = buffer.data() + dataOffset;

    constexpr auto frameTypeShift {4};

    // Fixed header
    FixedHeader header {};
    header.m_retain = false;
    header.m_qos = Qos::Qos1;
    header.m_type = static_cast<int>(FrameTypeTests::Subscribe) >> frameTypeShift;
    appendFixedHeader(tail, header);
    appendVariableLength(tail, packetLength);

    // Variable header
    if (messageId == 0)
    {
        throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Message id is 0");
    }
    appendShortValue(tail, messageId);

    // Properties
    appendProperties(tail, message.getProperties(), expectedPropertiesSize, 0, {});

    // Payload
    for (const auto& destination: destinations)
    {
        appendVariableHeader(tail, destination.m_topic->name().data(), static_cast<uint16_t>(destination.m_topic->size()));
        // Subscription options (one getByte)
        if (getProtocolVersion() == ProtocolVersion::MqttV5)
        {
            appendByte(tail, *bit_cast<uint8_t*>(&destination.m_subscribeOptions));
        }
        else
        {
            SubscriptionOptions subscribeOptions;
            subscribeOptions.m_maxQos = destination.m_subscribeOptions.m_maxQos;
            appendByte(tail, *bit_cast<uint8_t*>(&subscribeOptions));
        }
    }

    buffer.bytes(tail - buffer.data());
}

void MessageWriter::appendUnsubscribeToBuffer(Buffer& buffer, const UnsubscribeMessage& message, const MessageId messageId) const
{
    auto [expectedPropertiesSize, extraPacketSize] = computePropertiesMetrics(message.getProperties(), {});

    uint32_t            packetLength = 2 + extraPacketSize;
    const Destinations& destinations = message.destinations();
    for (const auto& destination: destinations)
    {
        constexpr auto topicLengthSize {2};
        packetLength += static_cast<uint32_t>(destination.m_topic->size() + topicLengthSize);
    }

    constexpr auto maxRemainingLengthSize {4};
    const auto     dataOffset = buffer.bytes();
    buffer.reserve(dataOffset + packetLength + maxRemainingLengthSize);
    uint8_t* tail = buffer.data() + dataOffset;

    constexpr auto frameTypeShift {4};

    // Fixed header
    FixedHeader header {};
    header.m_retain = false;
    header.m_qos = Qos::Qos1;
    header.m_type = static_cast<int>(FrameTypeTests::Unsubscribe) >> frameTypeShift;
    appendFixedHeader(tail, header);
    appendVariableLength(tail, packetLength);

    // Variable header
    if (messageId == 0)
    {
        throw ProtocolException(getProtocolVersion(), ReasonCode::MalformedPacket, "Message id is 0");
    }
    appendShortValue(tail, messageId);

    // Properties
    appendProperties(tail, message.getProperties(), expectedPropertiesSize, 0, {});

    // Payload
    for (const auto& destination: destinations)
    {
        appendVariableHeader(tail, destination.m_topic->name().data(), static_cast<uint16_t>(destination.m_topic->name().size()));
    }

    buffer.bytes(tail - buffer.data());
}

void MessageWriter::appendSubscribeAckToBuffer(Buffer& buffer, const uint16_t ackMessageId, const vector<uint8_t>& subscriptionResults,
                                               const SMessageProperties& subscribeAckProperties) const
{
    auto [expectedPropertiesSize, extraPacketSize] = computePropertiesMetrics(subscribeAckProperties, {});

    constexpr auto headerSize {5};
    const size_t   expectedLength = headerSize + subscriptionResults.size() + extraPacketSize;
    const auto     dataOffset = buffer.bytes();
    buffer.reserve(dataOffset + expectedLength);

    uint8_t* tail = buffer.data() + dataOffset;

    constexpr auto fixedHeader = static_cast<uint8_t>(FrameTypeTests::SubAck);

    const auto remainingLength = static_cast<uint32_t>(subscriptionResults.size()) + 2 + extraPacketSize;

    // Fixed header
    appendByte(tail, fixedHeader);
    appendVariableLength(tail, remainingLength);

    appendShortValue(tail, ackMessageId);

    // Properties
    appendProperties(tail, subscribeAckProperties, expectedPropertiesSize, 0, {});
    appendData(tail, subscriptionResults.data(), subscriptionResults.size());

    buffer.bytes(tail - buffer.data());
}

bool MessageWriter::appendMessageToBuffer(Buffer& buffer, const Message& message, const MessageFlags messageFlags,
                                          const MessageId& deliveryId, const uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                                          const size_t maxPacketSize) const
{
    bool sent = false;
    switch (message.type())
    {
        using enum Message::Type;
        case Publish:
            sent = appendPublishToBuffer(buffer, *bit_cast<const PublishMessage*>(&message), messageFlags, deliveryId,
                                         remainingExpirationSeconds, subscriptionIds, maxPacketSize);
            break;
        case PublishReceived:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PubRec), message.getId(), ReasonCode::Success);
            sent = true;
            break;
        case PublishRelease:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PubRel), message.getId(), ReasonCode::Success);
            sent = true;
            break;
        case PublishComplete:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PubComp), message.getId(), ReasonCode::Success);
            sent = true;
            break;
        case Subscribe:
            appendSubscribeToBuffer(buffer, *bit_cast<const SubscribeMessage*>(&message), deliveryId);
            sent = true;
            break;
        case Unsubscribe:
            appendUnsubscribeToBuffer(buffer, *bit_cast<const UnsubscribeMessage*>(&message), deliveryId);
            sent = true;
            break;
        case SubscribeAck:
            appendSubscribeAckToBuffer(buffer, message.getId(),
                                       bit_cast<const SubscribeAckMessage*>(&message)->subscriptionResults(),
                                       message.getProperties());
            sent = true;
            break;
        case UnsubscribeAck:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::UnsubAck), message.getId(), ReasonCode::Success);
            sent = true;
            break;
        case PingReq:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PingReq), 0, ReasonCode::Success);
            sent = true;
            break;
        case PingResp:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PingResp), 0, ReasonCode::Success);
            sent = true;
            break;
        case PublishAck:
            appendAckToBuffer(buffer, static_cast<int>(FrameTypeTests::PubAck), message.getId(), ReasonCode::Success);
            sent = true;
            break;
        case ConnectAck:
            if (const auto* connectMessage = dynamic_cast<const ConnectMessage*>(&message))
            {
                appendConnectAckToBuffer(buffer, {},
                                         connectMessage->getReasonCode(),
                                         connectMessage->getSessionPresent());
                sent = true;
            }
            break;
        case Disconnect:
            if (const auto* disconnectMessage = dynamic_cast<const DisconnectMessage*>(&message))
            {
                appendDisconnectToBuffer(buffer, disconnectMessage->getReasonCode());
                sent = true;
            }
            break;
        case Undefined:
        case Connect:
            throw Exception("MessageWriter doesn't support message type " + message.name());
    }
    return sent;
}

void MessageWriter::appendConnectAckToBuffer(Buffer& buffer, const SMessageProperties& connectMessageProperties, ReasonCode reasonCode, const bool sessionPresent) const
{
    constexpr auto fixedHeader = static_cast<uint8_t>(FrameTypeTests::ConnAck);
    auto [expectedPropertiesSize, extraPacketSize] = computePropertiesMetrics(connectMessageProperties, {});
    const auto packetLength = 2 + extraPacketSize;

    constexpr auto bufferSize(128);
    const auto     dataOffset = buffer.bytes();
    buffer.reserve(dataOffset + bufferSize);
    uint8_t* tail = buffer.data() + dataOffset;

    // Fixed header
    appendByte(tail, fixedHeader);

    // Remaining length
    appendVariableLength(tail, packetLength);

    appendByte(tail, sessionPresent ? 1 : 0);
    appendByte(tail, static_cast<uint8_t>(reasonCode));

    // Properties
    appendProperties(tail, connectMessageProperties, expectedPropertiesSize, 0, {});

    buffer.bytes(tail - buffer.data());
}

std::tuple<uint32_t, uint32_t> MessageWriter::computePropertiesMetrics(const SMessageProperties& messageProperties, const SubscriptionIdSet& subscriptionIds) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        auto expectedPropertiesSize = messageProperties ? messageProperties->expectedSize() : 0;
        for (const auto subscriptionId: subscriptionIds)
        {
            expectedPropertiesSize += VariableLength::bytes(subscriptionId) + 1;
        }
        const uint32_t propertiesRemainingLengthSize = VariableLength::bytes(expectedPropertiesSize);
        return {expectedPropertiesSize, expectedPropertiesSize + propertiesRemainingLengthSize};
    }
    return std::make_tuple(0, 0);
}

void MessageWriter::computePropertiesMetrics(const SMessageProperties& messageProperties, const SubscriptionIdSet& subscriptionIds, uint32_t& expectedPropertiesSize, uint32_t& extraPacketSize) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        expectedPropertiesSize = messageProperties ? messageProperties->expectedSize() : 0;
        for (const auto subscriptionId: subscriptionIds)
        {
            expectedPropertiesSize += VariableLength::bytes(subscriptionId) + 1;
        }
        const uint32_t propertiesRemainingLengthSize = VariableLength::bytes(expectedPropertiesSize);
        extraPacketSize = expectedPropertiesSize + propertiesRemainingLengthSize;
        return;
    }
    expectedPropertiesSize = 0;
    extraPacketSize = 0;
}

void MessageWriter::appendProperties(uint8_t*& tail, const SMessageProperties& messageProperties, const uint32_t expectedPropertiesSize, uint32_t,
                                     const SubscriptionIdSet& subscriptionIds) const
{
    if (getProtocolVersion() == ProtocolVersion::MqttV5)
    {
        appendVariableLength(tail, expectedPropertiesSize);
        if (messageProperties)
        {
            messageProperties->write(tail, subscriptionIds);
        }
        else
        {
            for (const auto& subscriptionId: subscriptionIds)
            {
                MessageProperties::writeVariableLengthIntegerProperty(Property::SubscriptionIdentifier, tail, subscriptionId);
            }
        }
    }
}

} // namespace xmq::mqtt
