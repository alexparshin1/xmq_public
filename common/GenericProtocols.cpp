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

#include "GenericProtocols.h"

using namespace std;
using namespace sptk;

namespace xmq {

GenericProtocols::GenericProtocols(const STopicManager& topicManager)
    : m_topicManager(topicManager)
    , m_messageReaders(make_shared<MessageReaders>(topicManager))
    , m_messageWriters(make_shared<MessageWriters>())
{
    m_protocols[0] = make_shared<GenericProtocol>(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV31);
    m_protocols[1] = make_shared<GenericProtocol>(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV311);
    m_protocols[2] = make_shared<GenericProtocol>(m_messageReaders, m_messageWriters, ProtocolVersion::MqttV5);
}

const GenericProtocol& GenericProtocols::getProtocol(const ProtocolVersion protocolVersion) const
{
    const auto index = static_cast<size_t>(protocolVersion) - static_cast<size_t>(ProtocolVersion::MqttV31);
    if (index > m_protocols.size())
    {
        throw Exception("Invalid protocol version");
    }
    return *m_protocols[index];
}

} // namespace xmq
