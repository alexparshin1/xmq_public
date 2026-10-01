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

#include "xmq.h"

#include "MessageFlags.h"
#include "MessageId.h"
#include "base/MessageProperties.h"

#include <sptk5/cutils>

namespace xmq {

/**
 * @brief Base class for messages.
 */
class XMQ_EXPORT Message : public MessageFlags
{
public:
    /**
     * @brief Message type.
     */
    enum class Type : uint8_t
    {
        Undefined = 0,
        Connect = 1,
        Disconnect = 2,
        Subscribe = 3,
        Unsubscribe = 4,
        PingReq = 5,
        Publish = 6,
        ConnectAck = 7,
        SubscribeAck = 8,
        PublishAck = 9,
        PublishReceived = 10,
        PublishRelease = 11,
        PublishComplete = 12,
        UnsubscribeAck = 13,
        PingResp = 14
    };

    /**
     * @brief Constructor.
     * @param type              Message type.
     */
    explicit Message(const Type type)
        : m_type(type)
    {
    }

    /**
     * @brief Constructor.
     * @param type              Message type.
     * @param messageId         Message ID.
     */
    explicit Message(const Type type, const MessageId messageId)
        : m_id(messageId)
        , m_type(type)
    {
    }

    /**
     * @brief Destructor.
     */
    virtual ~Message() = default;

    /**
     * @brief Get message type.
     */
    [[nodiscard]] Type type() const
    {
        return m_type;
    }

    /**
     * @brief Check if the message type matches the provided type.
     * @param type              Message type to compare with.
     * @return true if the message type matches, false otherwise.
     */
    [[nodiscard]] bool is(const Type type) const
    {
        return m_type == type;
    }

    /**
     * @brief Check if the message type matches any of the provided types.
     * @param types             List of message types to compare with.
     * @return true if the message type matches any of the provided types, false otherwise.
     */
    [[nodiscard]] bool isOneOf(const std::initializer_list<Type> types) const
    {
        return std::ranges::any_of(types, [this](const auto type)
                                   {
                                       return m_type == type;
                                   });
    }

    /**
     * @brief Get the message type name.
     */
    [[nodiscard]] std::string name() const
    {
        return messageTypeName(m_type);
    }

    /**
     * @brief Message type name by message type.
     * @param type             Message type.
     * @return message type name.
     */
    [[nodiscard]] static std::string messageTypeName(Type type);

    /**
     * @brief Message type by message type name
     * @param typeName          Message type name
     * @return message type
     */
    [[nodiscard]] static Type messageType(std::string_view typeName);

    /**
     * @brief Get message ID
     * @return message ID
     */
    [[nodiscard]] MessageId getId() const
    {
        return m_id;
    }

    /**
     * @brief Set message ID.
     * @param messageId         The message id.
     */
    void setId(const MessageId& messageId)
    {
        m_id = messageId;
    }

    /**
     * @brief Get the string representation of the message.
     * @return string representation of the message.
     */
    [[nodiscard]] virtual std::string toString() const
    {
        return "Abstract message";
    }

    /**
     * @brief Get message properties.
     * @return message properties.
     */
    [[nodiscard]] const SMessageProperties& getProperties() const
    {
        return m_properties;
    }

    /**
     * @brief Set message properties.
     * @param messageProperties Message properties.
     */
    void setProperties(const SMessageProperties& messageProperties)
    {
        m_properties = messageProperties;
    }

    /**
     * @brief Check if the message is expired.
     * @param remainingSeconds Remaining seconds before expiration.
     * @return true if the message is expired, false otherwise.
     */
    [[nodiscard]] bool isExpired(uint32_t& remainingSeconds) const
    {
        if (m_properties)
        {
            return m_properties->isExpired(remainingSeconds);
        }
        return false;
    }

private:
    MessageId          m_id {0};     ///< Message ID.
    Type               m_type;       ///< Message type.
    SMessageProperties m_properties; ///< Message properties (MQTT5 only).
};

constexpr Message::Type messageTypeToAckType(const Message::Type& messageType, const Qos qos)
{
    using enum Message::Type;
    auto ackType = Undefined;
    switch (messageType)
    {
        case Connect:
            ackType = ConnectAck;
            break;
        case Publish:
            ackType = qos == Qos::Qos2 ? PublishReceived : PublishAck;
            break;
        case PublishReceived:
            ackType = PublishRelease;
            break;
        case PublishRelease:
            ackType = PublishComplete;
            break;
        case Subscribe:
            ackType = SubscribeAck;
            break;
        case Unsubscribe:
            ackType = UnsubscribeAck;
            break;
        case Disconnect:
            ackType = Disconnect;
            break;
        case PingReq:
            ackType = PingResp;
            break;
        default:
            // No ack required
            break;
    }
    return ackType;
}

/**
 * @brief Message shared pointer type
 */
using SMessage = std::shared_ptr<Message>;

} // namespace xmq

std::ostream& operator<<(std::ostream& os, const xmq::Message& message);
