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

#include "GenericProtocol.h"
#include "common/PacketReader.h"
#include "common/mqtt/MessageReader.h"

using namespace std;
using namespace sptk;
using namespace chrono;
using namespace xmq;

GenericProtocol::GenericProtocol(const SMessageReaders& messageReaders, const SMessageWriters& messageWriters, const ProtocolVersion protocolVersion)
    : m_packetReader(PacketReader::factory(protocolVersion))
    , m_messageReader(messageReaders->create(protocolVersion))
    , m_messageWriter(messageWriters->create(protocolVersion))
    , m_protocolVersion(protocolVersion)
{
}
