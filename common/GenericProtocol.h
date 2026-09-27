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

#include "MessageReaders.h"
#include "MessageWriters.h"
#include "base/Topic.h"
#include "common/PacketReader.h"

namespace xmq {

class GenericConnection;

/**
 * @brief Generic protocol.
 * Combines packet reader, message reader and message writer that are defined by protocol version.
 * Only a single protocol instance is required for all the client connections that have the same protocol version
 */
class XMQ_EXPORT GenericProtocol final
{
public:
    /**
     * @brief Private constructor.
     * @remarks Use create() to create an object of this class.
     * @param messageReaders        Message readers.
     * @param messageWriters        Message writers.
     * @param protocolVersion       Protocol version.
     */
    GenericProtocol(const SMessageReaders& messageReaders, const SMessageWriters& messageWriters, ProtocolVersion protocolVersion);

    GenericProtocol() = default;
    GenericProtocol(const GenericProtocol&) = default;
    GenericProtocol(GenericProtocol&&) = default;
    GenericProtocol& operator=(const GenericProtocol&) = default;
    GenericProtocol& operator=(GenericProtocol&&) = default;

    /**
     * @brief Destructor.
     */
    ~GenericProtocol() = default;

    /**
     * @brief Protocol specific packet reader.
     * @return packet reader.
     */
    [[nodiscard]] const SPacketReader& packetReader() const
    {
        return m_packetReader;
    }

    /**
     * @brief Protocol specific message reader.
     * @return packet reader.
     */
    [[nodiscard]] const SMessageReader& messageReader() const
    {
        return m_messageReader;
    }

    /**
     * @brief Protocol specific message writer.
     * @return packet reader.
     */
    [[nodiscard]] const SMessageWriter& messageWriter() const
    {
        return m_messageWriter;
    }

    /**
     * @brief Protocol version.
     * @return Protocol version.
     */
    [[nodiscard]] ProtocolVersion version() const
    {
        return m_protocolVersion;
    }

private:
    SPacketReader   m_packetReader; ///< Packet reader
    SMessageReader  m_messageReader; ///< Message reader
    SMessageWriter  m_messageWriter; ///< Message writer
    ProtocolVersion m_protocolVersion{ProtocolVersion::MqttV31}; ///< Protocol version
};

using SGenericProtocol = std::shared_ptr<GenericProtocol>;

} // namespace xmq