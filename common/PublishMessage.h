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

#include "base/LatencyTrace.h"
#include "base/Message.h"
#include "base/Topic.h"

namespace xmq {

class ClientSession;

/**
 * Generic Publish message - used as a base class for all Publish messages
 */
class XMQ_EXPORT PublishMessage : public Message
{
public:
    /**
     * @brief Constructor.
     */
    PublishMessage()
        : Message(Type::Publish)
    {
    }

    /**
     * @brief Destructor.
     */
    ~PublishMessage() override = default;

    /**
     * @brief Get message sender.
     * @return message sender.
     */
    [[nodiscard]] const std::string& getSender() const
    {
        return m_sender;
    }

    /**
     * @brief Set message sender.
     * @param sender Message sender.
     */
    void setSender(const std::string_view sender)
    {
        m_sender = sender;
    }

    /**
     * @brief Get message destination.
     * @return message destination.
     */
    [[nodiscard]] virtual const Topic* destination() const = 0;

    /**
     * @brief Set the message destination.
     * @param destination       Message destination.
     */
    virtual void setDestination(const Topic* destination) = 0;

    /**
     * @brief Get the message payload.
     * @return message payload pointer.
     */
    [[nodiscard]] virtual std::string_view payload() const = 0;

    /**
     * @brief Get the message payload pointer.
     * @return message payload pointer.
     */
    [[nodiscard]] virtual const uint8_t* payloadData() const = 0;

    /**
     * @brief Get message payload size.
     * @return message payload size.
     */
    [[nodiscard]] virtual uint32_t payloadSize() const = 0;
    /**
     * @brief Get the source node name.
     * @return Source node name.
     */
    [[nodiscard]] virtual const std::string& getSourceNode() const = 0;

    /**
     * @brief Set the source node name.
     * The source node name is set when the message is received from the other node.
     * @param sourceNode Source node name.
     */
    virtual void setSourceNode(std::string_view sourceNode) = 0;

    [[nodiscard]] LatencyTrace* asTrace() const
    {
        const auto data = this->payload();
        if (data.size() < sizeof(LatencyTrace)) { return nullptr; }
        const auto* trace = reinterpret_cast<const LatencyTrace*>(data.data());
        return trace->m_signature == 0x5115 ? const_cast<LatencyTrace*>(trace) : nullptr;
    }

private:
    std::string m_sender; ///< Message sender.
};

using SPublishMessage = std::shared_ptr<PublishMessage>;

} // namespace xmq
