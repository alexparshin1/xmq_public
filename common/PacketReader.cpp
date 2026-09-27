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

#include "PacketReader.h"
#include "mqtt/PacketReaderMqtt.h"

using namespace std;
using namespace xmq;

SPacketReader PacketReader::factory(ProtocolVersion)
{
    static shared_ptr<mqtt::PacketReaderMqtt> mqttPacketReader;
    static shared_mutex                       mutex;

    {
        shared_lock lock(mutex);
        if (mqttPacketReader)
        {
            return mqttPacketReader;
        }
    }

    unique_lock lock(mutex);
    if (mqttPacketReader)
    {
        return mqttPacketReader;
    }
    mqttPacketReader = make_shared<mqtt::PacketReaderMqtt>();
    return mqttPacketReader;
}
