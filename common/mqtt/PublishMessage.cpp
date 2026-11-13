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

#include "PublishMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
constexpr auto messageIdSize = sizeof(uint16_t);
}

mqtt::PublishMessage::PublishMessage(const Topic* destination, const string_view data, const MessageId messageId, const bool retain)
    : m_messageData(destination->name().size() + data.size() + 2 * sizeof(uint16_t) + (messageId == 0 ? 0 : messageIdSize))
    , m_destination(destination)
{
    const auto& destinationBuffer = destination->name();
    auto*       writePosition = m_messageData.data() + sizeof(uint16_t);
    *bit_cast<uint16_t*>(m_messageData.data()) = htons(static_cast<uint16_t>(destinationBuffer.size()));
    memcpy(writePosition, destinationBuffer.data(), destinationBuffer.size());
    writePosition += destinationBuffer.size();

    if (messageId != 0)
    {
        *bit_cast<uint16_t*>(writePosition) = messageId;
        writePosition += sizeof(uint16_t);
    }

    m_payload = writePosition;
    m_payloadSize = static_cast<uint32_t>(data.size());
    memcpy(m_payload, data.data(), m_payloadSize);
    writePosition += m_payloadSize;

    m_messageData.resize(writePosition - m_messageData.data());

    setRetain(retain);
}

mqtt::PublishMessage::PublishMessage(const STopicManager& topicManager, TopicCache& topicCache, const FixedHeader& fixedHeader,
                                     Packet&& messageData, const ProtocolVersion protocolVersion)
    : m_messageData(std::move(messageData).takeData())
{
    const auto payloadSize = static_cast<uint32_t>(m_messageData.size());
    auto*      readPosition = m_messageData.data();
    auto       availableBytes = payloadSize;

    if (availableBytes < sizeof(uint16_t))
    {
        throw Exception("MQTT PUBLISH packet is missing the topic name length");
    }

    const auto destinationSize = ntohs(*bit_cast<uint16_t*>(readPosition));
    readPosition += sizeof(uint16_t);
    availableBytes -= sizeof(uint16_t);
    if (destinationSize > availableBytes)
    {
        throw Exception("MQTT PUBLISH packet has an invalid topic name length");
    }
    if (destinationSize == 0)
    {
        // MQTT 5 lets a PUBLISH carry no topic at all when a topic alias names the destination;
        // the reader resolves that after the properties are known, and refuses the message if the
        // alias names nothing. Nothing is cached meanwhile: there is no name to match later.
        m_destination = TopicManager::emptyTopic();
    }

    const string_view destination(bit_cast<char*>(readPosition), destinationSize);
    // The session's last topic serves this one unless the publisher moved on: see TopicCache.
    if (destinationSize != 0)
    {
        if (topicCache.m_topic != nullptr && destination == topicCache.m_name)
        {
            m_destination = topicCache.m_topic;
        }
        else
        {
            m_destination = topicManager->getTopic(destination);
            topicCache.m_name.assign(destination);
            topicCache.m_topic = m_destination;
        }
    }

    readPosition += destinationSize;
    availableBytes -= destinationSize;
    if (fixedHeader.m_qos != Qos::Qos0)
    {
        if (availableBytes < messageIdSize)
        {
            throw Exception("MQTT PUBLISH packet is missing its packet identifier");
        }
        const auto messageId = ntohs(*bit_cast<uint16_t*>(readPosition));
        readPosition += sizeof(uint16_t);
        availableBytes -= sizeof(uint16_t);
        setId(messageId);
    }

    if (protocolVersion == ProtocolVersion::MqttV5)
    {
        if (const auto propertiesLength = VariableLength::read(readPosition, static_cast<int>(availableBytes)))
        {
            const auto properties = make_shared<MessageProperties>();
            setProperties(properties);
            properties->read(readPosition, propertiesLength);
            readPosition += propertiesLength;
        }
    }

    m_payload = readPosition;
    m_payloadSize = payloadSize - static_cast<uint32_t>(readPosition - m_messageData.data());

    setQos(fixedHeader.m_qos);
    setRetain(fixedHeader.m_retain);
    setDup(fixedHeader.m_dup);
}

mqtt::PublishMessage::PublishMessage(const PublishMessage& other)
    : xmq::PublishMessage(other)
    , m_messageData(other.m_messageData)
    , m_destination(other.m_destination)
    , m_payloadSize(other.m_payloadSize)
{
    const auto payloadOffset = other.m_payload - other.m_messageData.data();
    m_payload = m_messageData.data() + payloadOffset;
}


string mqtt::PublishMessage::toString() const
{
    stringstream str;

    const auto   destinationLength = ntohs(*bit_cast<const uint16_t*>(m_messageData.data()));
    const auto*  destinationStart = bit_cast<const char*>(m_messageData.data()) + sizeof(uint16_t);
    const auto*  destinationEnd = destinationStart + destinationLength;
    const string destination(destinationStart, destinationEnd);

    constexpr size_t payloadPrintLimit = 80;
    const auto       truncatePayload = m_payloadSize > payloadPrintLimit;
    const string     message(bit_cast<const char*>(m_payload), truncatePayload ? payloadPrintLimit : m_payloadSize);

    str << "Publish id=" << getId()
        << " sender=" << getSender()
        << " qos=" << to_string(static_cast<int>(getQos()))
        << ", payload=" << m_payloadSize << " bytes, topic='" << destination
        << "'" << (isRetain() ? " isRetain" : "")
        << " [" << message << (truncatePayload ? ".." : "") << "]";

    if (getProperties())
    {
        str << *getProperties();
    }

    return str.str();
}
