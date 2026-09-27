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

#include "Qos.h"

using namespace std;
using namespace xmq;

std::string to_string(const Qos qos)
{
    using enum Qos;
    switch (qos)
    {
        case Qos0:
            return "QOS0";
        case Qos1:
            return "QOS1";
        case Qos2:
            return "QOS2";
        case Invalid:
        default:
            return "InvalidQOS";
    }
}
