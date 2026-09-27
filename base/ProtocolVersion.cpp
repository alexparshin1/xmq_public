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

#include "ProtocolVersion.h"

using namespace std;
using namespace sptk;

string to_string(const xmq::ProtocolVersion protocolVersion)
{
    switch (protocolVersion)
    {
        using enum xmq::ProtocolVersion;
        case MqttV31:
            return "MQTT 3.1";
        case MqttV311:
            return "MQTT 3.1.1";
        case MqttV5:
            return "MQTT 5";
    }
    return "Unknown protocol";
}
