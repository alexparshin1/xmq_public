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
#include "base/AckMessage.h"
#include "base/Message.h"
#include "common/BaseClientSession.h"
#include "common/MessageReader.h"
#include "common/Packet.h"
#include "FixedHeader.h"
#include "FrameTypeTests.h"
#include "PublishMessage.h"
namespace xmq::mqtt {
/**
 * @brief MQTT message reader.
 */
class XMQ_EXPORT MessageReader final : public xmq::MessageReader
{
public:
    using xmq::MessageReader::MessageReader;
    /**
     * @brief Constructor
     * @param topicManager      Topic manager.
     */
    explicit MessageReader(const STopicManager& topicManager, ProtocolVersion protocolVersion);
    /**
     * @brief Destructor.
     */
    ~MessageReader() override = default;
    /**
     * @brief Read a message from the socket.
     * @return Message.
     */
    [[nodiscard]] SMessage readMessage(Packet&& packet, BaseClientSession& clientSession, uint64_t packetReceivedTS) const override;
private:
    STopicManager m_topicManager;
    SAckMessage   m_ackMessage {std::make_shared<AckMessage>(Message::Type::Undefined, static_cast<MessageId>(0))}; ///< Generic Ack message
    static ReasonCode readLastWillInfo(Packet& packet, uint8_t protocolVersionByte, const SLastWillInfo& lastWill, ConnectFlags connectFlags);
    /**
     * @brief Read a 'Connect' message.
     * @returns Message.
     */
    static SMessage readConnect(Packet& packet, BaseClientSession& clientSession, uint64_t packetReceivedTS);
    /**
     * @brief Read a Publish message.
     * @param packet            Packet.
     * @param messageHeader     Message header.
     * @param clientSession     Client session.
     * @param packetReceivedTS  Timestamp: when the packet was received.
     * @return Message.
     */
    SMessage readPublish(Packet&& packet, const FixedHeader& messageHeader, BaseClientSession& clientSession, uint64_t packetReceivedTS) const;
    /**
     * @brief Read a Subscribe message.
     * @param packet            Packet.
     * @param remainingLength   Remaining message length.
     * @return Message.
     */
    SMessage readSubscribe(Packet& packet, uint32_t remainingLength) const;
    /**
     * @brief Read an Unsubscribe message.
     * @param packet.
     * @param remainingLength   Remaining message length.
     * @return Message.
     */
    SMessage readUnsubscribe(Packet& packet, uint32_t remainingLength) const;
    /**
     * @brief Read a Disconnect message.
     * @return Message.
     */
    SMessage readDisconnect(Packet& packet) const;
    /**
     * @brief Read Connect Ack message.
     * @return Message.
     */
    SMessage readConnectAck(Packet& packet) const;
    /**
     * @brief Read a Generic Ack message.
     * @param packet            Packet.
     * @param frameType         Frame type.
     * @return Message.
     */
    static SMessage readGenericAck(Packet& packet, FrameTypeTests frameType);
    /**
     * @brief Read a Subscribe Ack message.
     * @param packet            Packet.
     * @return Message.
     static static static static static */
    SMessage readSubscribeAck(Packet& packet) const;
    /**
     * @brief Read No Payload message.
     * @param frameType        Frame type.
     * @return Message.
     */
    static SMessage readNoPayloadMessage(FrameTypeTests frameType);
    /**
     * @brief Read properties (MQTT 5 only).
     * @return tuple of properties and number of bytes read.
     */
    static std::tuple<SMessageProperties, uint32_t> readPropertiesMqtt5(Packet& packet);
    /**
     * @brief Read MQTT5 message properties.
     * @return total size taken by properties, including properties' length.
     */
    size_t readProperties(Packet& packet, Message* message) const override;
};
} // namespace xmq::mqtt
