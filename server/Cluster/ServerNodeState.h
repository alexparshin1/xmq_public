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

#include "base/Message.h"
#include "client/MqttClient.h"
#include "service/CServerNode.h"

namespace xmq::cluster {

/**
 * @brief Server node state.
 */
enum class ServerNodeState : uint8_t
{
    Offline = 1,
    Standalone,
    ConnectingToCluster,
    ConnectedToCluster,
    Disconnected
};

} // namespace xmq::cluster

std::string XMQ_EXPORT to_string(xmq::cluster::ServerNodeState state);
