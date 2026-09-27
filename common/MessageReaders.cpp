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

#include "MessageReaders.h"

#include "common/mqtt/MessageReader.h"
#include <utility>

using namespace std;
using namespace sptk;
using namespace xmq;

MessageReaders::MessageReaders(STopicManager topicManager)
    : m_topicManager(std::move(topicManager))
{
    using enum ProtocolVersion;
    static const vector protocolVersions {
        MqttV31,
        MqttV311,
        MqttV5};

    unique_lock lock(m_readerMutex);

    for (auto protocolVersion: protocolVersions)
    {
        const auto mqttReader = make_shared<mqtt::MessageReader>(m_topicManager, protocolVersion);
        m_readers[protocolVersion] = mqttReader;
    }
}

SMessageReader MessageReaders::create(const ProtocolVersion protocolVersion) const
{
    shared_lock lock(m_readerMutex);

    if (const auto it = m_readers.find(protocolVersion);
        it != m_readers.end())
    {
        return it->second;
    }

    throw Exception("Unknown protocol version");
}
