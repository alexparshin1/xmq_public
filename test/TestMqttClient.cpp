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

#include "TestMqttClient.h"

using namespace std;
using namespace sptk;

namespace xmq {

TestMqttClient::TestMqttClient(const std::shared_ptr<LogEngine>& logEngine, std::string_view clientId, const bool cleanSession, const bool anonymous,
                               const ProtocolVersion protocolVersion, const SMessageProperties& messageProperties, const uint16_t portNumber,
                               const bool useTls)
    : MqttClient(logEngine)
{
    const ConnectCredentials credentials {string(clientId), anonymous ? "" : "user", anonymous ? "" : "secret"};
    // No client certificate: the listener does not ask for one.
    const auto sslKeys = useTls ? make_shared<SSLKeys>() : nullptr;
    const auto port = useTls ? ServerTests_Suite::TestSslPortNumber : portNumber;
    connect(Host("localhost", port), credentials, {.m_cleanSession = cleanSession}, protocolVersion, messageProperties, sslKeys);
}

void TestMqttClient::publishMultiple(const std::string_view topic, const Buffer& payload, const size_t count, const Qos qos)
{
    for (size_t i = 0; i < count; ++i)
    {
        publish(getTopic(topic), payload, qos);
    }
}

} // namespace xmq
