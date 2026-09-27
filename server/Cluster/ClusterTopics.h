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

#include "base/Topic.h"

namespace xmq::cluster {

enum class Command
{
    Unknown,
    AttachNodeRequest,
    AttachNodeResponse,
    AttachNode,
    DetachNodeRequest,
    DetachNodeResponse,
    DisconnectClient,
    NodeDetached
};

class Topics
{
public:
    explicit Topics(const STopicManager& topicManager);

    [[nodiscard]] const Topic*     getTopic(Command command) const;
    [[nodiscard]] std::string_view getTopicName(Command command) const;
    Command                        getCommand(const Topic* topic) const;

private:
    const std::map<std::string_view, Command> m_topicCommand;
    const std::map<Command, const Topic*>     m_commandTopic;
};

} // namespace xmq::cluster
