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

#include "MessageWriter.h"
#include "Packet.h"

namespace xmq {

class BaseClientSession;
/**
 * Generic message reader - base class for message readers
 */
class XMQ_EXPORT MessageReader : public BaseProtocolVersion
{
public:
    /**
     * @brief Constructor.
     */
    explicit MessageReader(ProtocolVersion protocolVersion)
        : BaseProtocolVersion(protocolVersion)
    {
    }

    /**
     * @brief Deleted copy constructor.
     */
    MessageReader(const MessageReader&) = delete;

    /**
     * @brief Deleted move constructor.
     */
    MessageReader(MessageReader&&) = delete;

    /**
     * @brief Deleted copy assignment.
     */
    MessageReader& operator=(const MessageReader&) = delete;

    /**
     * @brief Deleted move assignment.
     */
    MessageReader& operator=(MessageReader&&) = delete;

    /**
     * @brief Destructor.
     */
    virtual ~MessageReader() = default;

    /**
     * @brief Read a single message.
     * @param packet            Source packet.
     * @param clientSession     Client session.
     * @param packetReceivedTS  Timestamp: when the packet was received.
     * @return true if the message is read.
     */
    [[nodiscard]] virtual SMessage readMessage(Packet&& packet, BaseClientSession& clientSession, uint64_t packetReceivedTS) const = 0;

protected:
    /**
     * @brief Read MQTT5 message properties
     * @remarks Does not read properties for MQTT3
     * @return total size taken by properties, including properties' length
     */
    virtual size_t readProperties(Packet& packet, Message* message) const = 0;
};

/**
 * @brief Generic message reader unique pointer type
 */
using SMessageReader = std::shared_ptr<MessageReader>;

} // namespace xmq
