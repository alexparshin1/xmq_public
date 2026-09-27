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

#include "MessageWriters.h"
#include "mqtt/MessageWriter.h"

using namespace std;
using namespace sptk;
using namespace xmq;

MessageWriters::MessageWriters()
{
    using enum ProtocolVersion;
    static const vector protocolVersions {
        MqttV31,
        MqttV311,
        MqttV5};

    unique_lock lock(m_writersMutex);

    for (auto protocolVersion: protocolVersions)
    {
        const auto mqttReader = make_shared<mqtt::MessageWriter>(protocolVersion);
        m_writers[protocolVersion] = mqttReader;
    }
}

SMessageWriter MessageWriters::create(const ProtocolVersion protocolVersion) const
{
    shared_lock lock(m_writersMutex);

    if (const auto iterator = m_writers.find(protocolVersion);
        iterator != m_writers.end())
    {
        return iterator->second;
    }
    throw Exception("Unknown protocol version");
}
