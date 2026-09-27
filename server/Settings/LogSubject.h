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

#pragma once

#include <base/xmq.h>

namespace xmq {
enum class LogSubject
{
    Ack = 0,
    Publish = 1,
    Subscribe = 2,
    Unsubscribe = 3,
    Connect = 4,
    Disconnect = 5,
    ServerConnections = 6,
    ServerEvents = 7,
    SessionErrors = 8,
    ClusterConnections = 9,
    ClusterEvents = 10,
    StorageEvents = 11
};

std::string logSubjectToString(LogSubject subject);

} // namespace xmq

std::string to_string(xmq::LogSubject subject);
