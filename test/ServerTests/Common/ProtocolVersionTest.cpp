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

#include "base/ProtocolVersion.h"

#include <gtest/gtest.h>

using namespace xmq;

TEST(ProtocolVersionTest, to_string)
{
    EXPECT_EQ(to_string(ProtocolVersion::MqttV31), "MQTT 3.1");
    EXPECT_EQ(to_string(ProtocolVersion::MqttV311), "MQTT 3.1.1");
    EXPECT_EQ(to_string(ProtocolVersion::MqttV5), "MQTT 5");
}
