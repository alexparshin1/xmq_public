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

#include "common/PacketReader.h"
#include <sptk5/cutils>

namespace xmq::mqtt {

class PacketReaderMqtt final : public PacketReader
{
public:
    /**
     * @brief Constructor.
     */
    PacketReaderMqtt() = default;

    /**
     * @brief Destructor.
     */
    ~PacketReaderMqtt() override = default;

    [[nodiscard]] ProtocolVersion protocolVersion() const override;
    [[nodiscard]] Packet          readPacket(sptk::TCPSocket* socket) const override;
    [[nodiscard]] Packet          readPacket(const sptk::Buffer& buffer, bool& empty, size_t& offset) const override;

private:
    /**
     * @brief Read the second and the following bytes of variable length.
     * @return variable length.
     */
    [[nodiscard]] static uint64_t readVariableLength(sptk::TCPSocket* socket, uint8_t& bytes);
    [[nodiscard]] static uint32_t readVariableLength(const sptk::Buffer& buffer, size_t offset);
};

} // namespace xmq::mqtt
