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

#include "ServerNodeState.h"

using namespace xmq::cluster;

std::string to_string(const ServerNodeState state)
{
    switch (state)
    {
        using enum ServerNodeState;
        case Offline:
            return "Offline";
        case Standalone:
            return "Standalone";
        case ConnectingToCluster:
            return "ConnectingToCluster";
        case ConnectedToCluster:
            return "ConnectedToCluster";
        case Disconnected:
            return "Disconnected";
    }
    return "Unknown";
}
