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
#include "Packet.h"

using namespace xmq;

Packet Packet::clone() const
{
    Packet packet;
    packet.m_data = m_data;
    packet.m_headerSize = m_headerSize;
    packet.m_header = m_header;
    packet.m_offset = m_offset;
    return packet;
}

uint32_t Packet::readVariableLength(unsigned multiplier)
{
    uint8_t  digit;
    uint32_t value = 0;

    constexpr auto valueContinuation = 128;

    do
    {
        constexpr uint8_t valueMaskInByte = 127;
        digit = readByte();
        value += (digit & valueMaskInByte) * multiplier;
        multiplier *= valueContinuation;
    } while ((digit & valueContinuation) != 0);

    return value;
}
