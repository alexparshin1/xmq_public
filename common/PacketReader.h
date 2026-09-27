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

#include "Packet.h"
#include "SocketDataReader.h"
#include "base/ProtocolVersion.h"
#include <sptk5/Buffer.h>

namespace xmq {

/**
 * Generic message reader - base class for message readers
 */
class XMQ_EXPORT PacketReader
{
public:
    /**
     * @brief Constructor.
     */
    explicit PacketReader() = default;

    PacketReader(const PacketReader&) = delete;
    PacketReader(PacketReader&&) = delete;
    PacketReader& operator=(const PacketReader&) = delete;
    PacketReader& operator=(PacketReader&&) = delete;

    /**
     * @brief Destructor.
     */
    virtual ~PacketReader() = default;

    [[nodiscard]] virtual ProtocolVersion protocolVersion() const = 0;

    /**
     * @brief Read a single message packet from the socket.
     * @param socket            Socket to read from.
     * @return Packet
     */
    [[nodiscard]] virtual Packet readPacket(sptk::TCPSocket* socket) const = 0;

    /**
     * @brief Read a single message packet from the buffer
     * @param buffer            Buffer to read from
     * @param empty             If it's true, then the packet isn't read
     * @param offset            Current buffer offset
     * @return Packet
     */
    [[nodiscard]] virtual Packet readPacket(const sptk::Buffer& buffer, bool& empty, size_t& offset) const = 0;

    template<typename T>
        requires(std::is_trivially_copyable_v<T> || std::is_enum_v<T>)
    [[nodiscard]] static bool tryRead(sptk::TCPSocket* socket, T& data)
    {
        if (socket->socketBytes() >= sizeof(data))
        {
            socket->read(reinterpret_cast<uint8_t*>(&data), sizeof(data));
            return true;
        }
        return false;
    }

    /**
     * @brief Message reader factory
     * @param protocolVersion   MQTT protocol version
     * @return message reader
     */
    static std::shared_ptr<PacketReader> factory(ProtocolVersion protocolVersion);
};

/**
 * @brief Generic message reader unique pointer type
 */
using SPacketReader = std::shared_ptr<PacketReader>;

} // namespace xmq
