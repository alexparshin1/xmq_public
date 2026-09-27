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

#include "PacketReaderMqtt.h"
#include "FixedHeader.h"

using namespace std;
using namespace sptk;

namespace xmq::mqtt {

ProtocolVersion PacketReaderMqtt::protocolVersion() const
{
    return ProtocolVersion::MqttV31;
}

size_t PacketReaderMqtt::readVariableLength(TCPSocket* socket, uint8_t& bytes)
{
    std::byte digit;
    size_t    value = 0;

    constexpr unsigned  initialMultiplier {128};
    constexpr std::byte valueContinuation {128};

    auto multiplier = initialMultiplier;

    do
    {
        constexpr std::byte valueMaskInByte {127};
        socket->read(reinterpret_cast<uint8_t*>(&digit), 1);
        value += static_cast<size_t>(digit & valueMaskInByte) * multiplier;
        multiplier *= static_cast<size_t>(valueContinuation);
        ++bytes;
    } while ((digit & valueContinuation) != static_cast<std::byte>(0));

    return value;
}

uint32_t PacketReaderMqtt::readVariableLength(const Buffer& buffer, size_t offset)
{
    std::byte digit;
    uint32_t  value = 0;

    constexpr unsigned  initialMultiplier {128};
    constexpr std::byte valueContinuation {128};

    auto multiplier = initialMultiplier;

    do
    {
        constexpr std::byte valueMaskInByte {127};
        digit = static_cast<std::byte>(buffer[offset]);
        ++offset;
        value += static_cast<uint32_t>(digit & valueMaskInByte) * multiplier;
        multiplier *= static_cast<uint32_t>(valueContinuation);
    } while ((digit & valueContinuation) != static_cast<std::byte>(0));

    return value;
}

Packet PacketReaderMqtt::readPacket(TCPSocket* socket) const
{
    static constexpr auto     readTimeout = 10s;
    static constexpr uint32_t valueContinuation = 128;
    static constexpr uint32_t valueMaskInByte = 127;

    // Read the fixed header and the first getByte of the remaining length
    if (FixedHeader packetHeader {};
        tryRead(socket, packetHeader))
    {
        uint8_t  extraRemainingLengthBytes = 0;
        uint64_t remainingLength = packetHeader.m_remainingLength;

        if ((remainingLength & valueContinuation) != 0)
        {
            remainingLength &= valueMaskInByte;
            remainingLength += readVariableLength(socket, extraRemainingLengthBytes);
        }

        Packet packet(bit_cast<uint8_t*>(&packetHeader), sizeof(FixedHeader),
                      remainingLength);
        readWithTimeout(*socket, packet.data(), remainingLength, readTimeout);
        packet.bytes(remainingLength);
        packet.setHeaderSize(sizeof(FixedHeader) + extraRemainingLengthBytes);

        return packet;
    }

    return Packet {};
}

Packet PacketReaderMqtt::readPacket(const Buffer& buffer, bool& empty, size_t& offset) const
{
    constexpr uint32_t valueContinuation = 128;
    constexpr uint32_t valueMaskInByte = 127;

    const size_t bufferSize = buffer.bytes();

    // Need at least the fixed header (type byte + first byte of the remaining length).
    if (offset + sizeof(FixedHeader) > bufferSize)
    {
        empty = true;
        return Packet {};
    }

    FixedHeader packetHeader {};
    memcpy(&packetHeader, buffer.data() + offset, sizeof(FixedHeader));

    // Decode the remaining-length varint. Its first byte is already in the fixed header.
    uint64_t remainingLength = packetHeader.m_remainingLength & valueMaskInByte;
    uint8_t  extraRemainingLengthBytes = 0;

    if ((packetHeader.m_remainingLength & valueContinuation) != 0)
    {
        uint64_t multiplier = valueContinuation;
        size_t   pos = offset + sizeof(FixedHeader);
        uint8_t  digit = 0;
        do
        {
            if (pos >= bufferSize)
            {
                // The variable-length field is not fully buffered yet.
                empty = true;
                return Packet {};
            }
            digit = buffer[pos];
            ++pos;
            ++extraRemainingLengthBytes;
            remainingLength += static_cast<uint64_t>(digit & valueMaskInByte) * multiplier;
            multiplier *= valueContinuation;
        } while ((digit & valueContinuation) != 0);
    }

    const size_t headerSize = sizeof(FixedHeader) + extraRemainingLengthBytes;

    // Is the whole packet (header + payload) buffered?
    if (offset + headerSize + remainingLength > bufferSize)
    {
        empty = true;
        return Packet {};
    }

    Packet packet(buffer.data() + offset, sizeof(FixedHeader), buffer.data() + offset + headerSize,
                  remainingLength);
    packet.setHeaderSize(headerSize);

    offset += headerSize + remainingLength;
    empty = false;
    return packet;
}

} // namespace xmq::mqtt
