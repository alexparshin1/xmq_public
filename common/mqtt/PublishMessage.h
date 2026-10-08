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
#include "VariableLength.h"
#include "base/ProtocolVersion.h"
#include "common/Packet.h"
#include "common/PublishMessage.h"
#include "base/MessageProperties.h"

#include <cstdint>
#include <vector>

namespace xmq::mqtt {

/**
 * @brief The MQTT 'Publish' message.
 */
class XMQ_EXPORT PublishMessage final : public xmq::PublishMessage
{
public:
    /**
     * @brief Constructor.
     * @param destination       Destination (topic name).
     * @param data              Message data.
     * @param messageId         Message messageId.
     * @param retain            Retain flag.
     */
    PublishMessage(const Topic* destination, std::string_view data, MessageId messageId = 0, bool retain = false);

    /**
     * @brief Constructor.
     * Constructs the Publish message from MQTT Publish message binary presentation (after fixed header).
     * @param topicManager      TopicManager.
     * @param fixedHeader       Fixed header.
     * @param messageData       Message data.
     * @param protocolVersion   MQTT protocol version.
     */
    PublishMessage(const STopicManager& topicManager, const FixedHeader& fixedHeader, Packet&& messageData, ProtocolVersion protocolVersion);

    /**
     * @brief Copy constructor.
     */
    PublishMessage(const PublishMessage& other);

    /**
     * @brief Destructor.
     */
    ~PublishMessage() override = default;

    /**
     * @brief Returns message destination (topic name).
     * @return Message destination (topic name).
     */
    [[nodiscard]] const Topic* destination() const override
    {
        return m_destination;
    }

    /**
     * @brief Set message destination.
     * @param destination       Message destination.
     */
    void setDestination(const Topic* destination) override
    {
        m_destination = destination;
    }

    /**
     * @brief Returns message string presentation.
     * @return Message string presentation.
     */
    [[nodiscard]] std::string toString() const override;

    /**
     * @brief Get the message payload.
     * @return message payload pointer.
     */
    [[nodiscard]] std::string_view payload() const override
    {
        return {reinterpret_cast<const char*>(m_payload), m_payloadSize};
    }

    /**
     * @brief Get the message payload pointer.
     * @return Message payload pointer.
     */
    [[nodiscard]] const uint8_t* payloadData() const override
    {
        return m_payload;
    }

    /**
     * @brief Get message payload size.
     * @return Message payload size.
     */
    [[nodiscard]] uint32_t payloadSize() const override
    {
        return m_payloadSize;
    }

    /**
     * @brief Get the source node name.
     * @return Source node name.
     */
    const std::string& getSourceNode() const override
    {
        return m_sourceNode;
    }

    /**
     * @brief Set the source node name.
     * The source node name is set when the message is received from the other node.
     * @param sourceNode Source node name.
     */
    void setSourceNode(const std::string_view sourceNode) override
    {
        m_sourceNode = sourceNode;
    }

private:
    std::vector<uint8_t> m_messageData;
    const Topic*         m_destination;
    uint8_t*             m_payload {nullptr};
    uint32_t             m_payloadSize {0};
    std::string          m_sourceNode;
};

using SPublishMessage = std::shared_ptr<PublishMessage>;

} // namespace xmq::mqtt
