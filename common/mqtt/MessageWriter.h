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

#pragma once

#include "FixedHeader.h"
#include "FrameTypeTests.h"
#include "SubscriptionIds.h"
#include "base/Message.h"
#include "common/ConnectMessage.h"
#include "common/MessageWriter.h"
#include "common/PublishMessage.h"
#include "common/SubscribeMessage.h"
#include "common/UnsubscribeMessage.h"
#include "base/MessageProperties.h"
#include <sptk5/net/TCPSocket.h>

namespace xmq::mqtt {

/**
 * @brief MQTT message writer.
 * This class is used for writing MQTT messages into buffer
 * and sending them to the socket.
 */
class XMQ_EXPORT MessageWriter final : public xmq::MessageWriter
{
public:
    using xmq::MessageWriter::MessageWriter;

    MessageWriter(const MessageWriter&) = delete;
    MessageWriter(MessageWriter&&) = delete;
    MessageWriter& operator=(const MessageWriter&) = delete;
    MessageWriter& operator=(MessageWriter&&) = delete;

    ~MessageWriter() override = default;

    /**
     * @brief Write the message into the buffer.
     * @param buffer            Output buffer.
     * @param message           Message.
     * @param messageFlags      Message delivery flags.
     * @param deliveryId        Message id.
     * @param remainingExpirationSeconds Remaining expiration seconds.
     * @param subscriptionIds   Matched subscription ids.
     * @param maxPacketSize     Maximum packet size.
     */
    bool appendMessageToBuffer(sptk::Buffer& buffer, const Message& message, MessageFlags messageFlags,
                               const MessageId& deliveryId, uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                               size_t maxPacketSize) const override;

    /**
     * @brief Write ack to buffer.
     * @param messageBuffer            Session socket.
     * @param frameType         Frame type.
     * @param ackMessageId      Message id.
     */
    void appendAckToBuffer(sptk::Buffer& messageBuffer, int frameType, const MessageId& ackMessageId, ReasonCode reasonCode) const override;

    /**
     * @brief Write ack to buffer.
     * @param messageBuffer            Session socket.
     * @param message     Source message.
     */
    void appendAckToBuffer(sptk::Buffer& messageBuffer, const Message* message, ReasonCode reasonCode) const override;

    /**
     * @brief Send connect message.
     * @param messageBuffer            Connect socket.
     * @param connectMessage    Connect message.
     * @param tweakMessage      Tweak message callback (for testing purposes).
     */
    void writeConnect(sptk::Buffer& messageBuffer, const ConnectMessage* connectMessage, const TweakMessageCallback& tweakMessage) const override;

    /**
     * @brief Append the Publish message to the buffer.
     * @param buffer            Message buffer.
     * @param message           Publish message.
     * @param messageFlags      Message delivery flags.
     * @param deliveryId        Message id.
     * @param remainingExpirationSeconds Remaining expiration seconds.
     * @param subscriptionIds   Matched subscriptions ids.
     * @param maxPacketSize     Maximum packet size.
     */
    bool appendPublishToBuffer(sptk::Buffer& buffer, const xmq::PublishMessage& message, MessageFlags messageFlags,
                               const MessageId& deliveryId, uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds,
                               size_t maxPacketSize) const override;

    /**
     * @brief Append subscribe message to buffer.
     * @param buffer     Message buffer.
     * @param message           Subscribe message.
     * @param messageId         Session's delivery id.
     */
    void appendSubscribeToBuffer(sptk::Buffer& buffer, const SubscribeMessage& message, MessageId messageId) const override;

    /**
     * @brief Write unsubscribe message to buffer.
     * @param buffer     Message buffer.
     * @param message           Unsubscribe message.
     * @param messageId         Session's delivery id.
     */
    void appendUnsubscribeToBuffer(sptk::Buffer& buffer, const UnsubscribeMessage& message, MessageId messageId) const override;

    /**
     * @brief Write disconnect message to buffer.
     * @param messageBuffer            Message buffer.
     */
    void appendDisconnectToBuffer(sptk::Buffer& messageBuffer, ReasonCode reasonCode) const override;

    /**
     * @brief Write subscribe ack into the buffer.
     * @param buffer                    Output buffer.
     * @param ackMessageId              Message id.
     * @param subscriptionResults          Granted QoS vector.
     * @param subscribeAckProperties    Subscribe ack properties.
     */
    void appendSubscribeAckToBuffer(sptk::Buffer& buffer, uint16_t ackMessageId, const std::vector<unsigned char>& subscriptionResults,
                                    const SMessageProperties& subscribeAckProperties) const;

    /**
     * @brief Write connect ack to buffer.
     * @param buffer     Message buffer.
     * @param connectMessageProperties Connect message properties.
     * @param reasonCode        Response code.
     * @param sessionPresent    Session was present.
     */
    void appendConnectAckToBuffer(sptk::Buffer& buffer, const SMessageProperties& connectMessageProperties,
                                  ReasonCode reasonCode, bool sessionPresent) const override;

    /**
     * @brief Send ping request.
     * @param messageBuffer            Session socket.
     */
    void appendPingRequestToBuffer(sptk::Buffer& messageBuffer) const override
    {
        appendAckToBuffer(messageBuffer, static_cast<int>(FrameTypeTests::PingReq), 0, ReasonCode::QuotaExceeded);
    }

    /**
     * @brief Send ping response.
     * @param messageBuffer            Session socket.
     */
    void appendPingResponseToBuffer(sptk::Buffer& messageBuffer) const override
    {
        appendAckToBuffer(messageBuffer, static_cast<int>(FrameTypeTests::PingResp), 0, ReasonCode::QuotaExceeded);
    }

    /**
     * Append string as 16-bit length (Big Endian) followed by the string characters
     * @param tail              The current end of data
     * @param data              Character string
     */
    static void appendString(uint8_t*& tail, const std::string& data)
    {
        appendShortValue(tail, static_cast<uint16_t>(data.length()));
        if (!data.empty())
        {
            appendData(tail, std::bit_cast<const uint8_t*>(data.c_str()), static_cast<uint16_t>(data.length()));
        }
    }

    /**
     * Append string as 16 bit length (Big Endian) followed by the string characters
     * @param tail              The current end of data
     * @param data              Character string
     */
    static void appendString(uint8_t*& tail, const std::string_view data)
    {
        appendShortValue(tail, static_cast<uint16_t>(data.length()));
        if (!data.empty())
        {
            appendData(tail, std::bit_cast<const uint8_t*>(data.data()), static_cast<uint16_t>(data.length()));
        }
    }

private:
    static constexpr auto m_initialSendBufferSize = 64;
    sptk::Buffer          m_sendBuffer {m_initialSendBufferSize};

    /**
     * @brief Compute properties size and message size increase.
     * @remarks For MQTT3, both these numbers are always 0.
     * @param messageProperties Message properties.
     * @param subscriptionIds   Subscription IDs.
     * @return tuple that contains properties size and message size increase.
     */
    [[nodiscard]] std::tuple<uint32_t, uint32_t> computePropertiesMetrics(const SMessageProperties& messageProperties, const SubscriptionIdSet& subscriptionIds) const;

    /**
     * @brief Compute properties size and message size increase.
     * @remarks For MQTT3, both these numbers are always 0.
     * @param messageProperties Message properties.
     * @param subscriptionIds   Subscription IDs.
     * @return tuple that contains properties size and message size increase.
     */
    void computePropertiesMetrics(const SMessageProperties& messageProperties, const SubscriptionIdSet& subscriptionIds,
                                  uint32_t& expectedPropertiesSize, uint32_t& extraPacketSize) const;

    /**
     * @brief Append fixed header to message data.
     * @remarks Does nothing for MQTT3.
     * @param tail                      The current end of data.
     * @param messageProperties         Message properties.
     * @param expectedPropertiesSize    Expected properties size.
     * @param remainingExpirationSeconds Remaining message expiration seconds.
     * @param subscriptionIds           Subscription IDs.
     */
    void appendProperties(uint8_t*& tail, const SMessageProperties& messageProperties, uint32_t expectedPropertiesSize,
                          uint32_t remainingExpirationSeconds, const SubscriptionIdSet& subscriptionIds) const;

    /**
     * @brief Append fixed header to message data.
     * @param tail              The current end of data.
     * @param fixedHeader       Fixed header.
     */
    static void appendFixedHeader(uint8_t*& tail, FixedHeader fixedHeader)
    {
        *tail = *std::bit_cast<uint8_t*>(&fixedHeader);
        ++tail;
    }

    /**
     * @brief Append getByte to message data.
     * @param tail              The current end of data.
     * @param tail              Pointer to the message data tail.
     * @param byte              Byte.
     */
    static void appendByte(uint8_t*& tail, const uint8_t byte)
    {
        *tail = byte;
        ++tail;
    }

    /**
     * @brief Append variant data to message data.
     * @param tail              Pointer to the message data tail.
     * @param data              Variant data.
     * @param dataLength        Variant data length.
     */
    static void appendData(uint8_t*& tail, const uint8_t* data, const size_t dataLength)
    {
        memcpy(tail, data, dataLength);
        tail += dataLength;
    }

    /**
     * Append short (16 bit) as Big Endian to the frame buffer
     * @param tail              The current end of data
     * @param value             16-bit integer
     */
    static void appendShortValue(uint8_t*& tail, const uint16_t value)
    {
        *std::bit_cast<uint16_t*>(tail) = htons(value);
        tail += sizeof(uint16_t);
    }

    /**
     * Append string as 16 bit length (Big Endian) followed by the string characters
     * @param tail              The current end of data
     * @param data              Character string
     * @param dataLength        Character string length
     */
    static void appendVariableHeader(uint8_t*& tail, const char* data, uint16_t dataLength);

    /**
     * Append 'remaining length' using the multi-getByte presentation MQTT algorithm
     * @param tail                  The current end of data
     * @param variableLengthValue   Remaining length
     */
    static void appendVariableLength(uint8_t*& tail, uint32_t variableLengthValue);
};

} // namespace xmq::mqtt
