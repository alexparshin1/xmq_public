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

#include "ClusterTopics.h"

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace cluster;

namespace {
constexpr string_view AttachNodeRequest = "$CLUSTER/request/join";
constexpr string_view AttachNodeResponse = "$CLUSTER/request/join_response";
constexpr string_view DetachNodeRequest = "$CLUSTER/request/detach_request";
constexpr string_view DetachNodeResponse = "$CLUSTER/request/detach_response";
constexpr string_view DisconnectClient = "$CLUSTER/request/disconnect_client";
constexpr string_view NodeDetached = "$CLUSTER/request/node_detached";
} // namespace

Topics::Topics(const STopicManager& topicManager)
    : m_topicCommand {
          {AttachNodeRequest, Command::AttachNodeRequest},
          {AttachNodeResponse, Command::AttachNodeResponse},
          {DetachNodeRequest, Command::DetachNodeRequest},
          {DetachNodeResponse, Command::DetachNodeResponse},
          {DisconnectClient, Command::DisconnectClient},
          {NodeDetached, Command::NodeDetached},
      }
    , m_commandTopic {
          {Command::AttachNodeRequest, topicManager->getTopic(AttachNodeRequest)},
          {Command::AttachNodeResponse, topicManager->getTopic(AttachNodeResponse)},
          {Command::DetachNodeRequest, topicManager->getTopic(DetachNodeRequest)},
          {Command::DetachNodeResponse, topicManager->getTopic(DetachNodeResponse)},
          {Command::DisconnectClient, topicManager->getTopic(DisconnectClient)},
          {Command::NodeDetached, topicManager->getTopic(NodeDetached)},
      }
{
}

const Topic* Topics::getTopic(const Command command) const
{
    if (const auto it = m_commandTopic.find(command);
        it != m_commandTopic.end())
    {
        return it->second;
    }
    return nullptr;
}

string_view Topics::getTopicName(const Command command) const
{
    if (const auto* topic = getTopic(command))
    {
        return topic->fullName();
    }
    throw Exception("Cluster topic not found");
}

Command Topics::getCommand(const Topic* topic) const
{
    if (const auto it = m_topicCommand.find(topic->fullName());
        it != m_topicCommand.end())
    {
        return it->second;
    }
    return Command::Unknown;
}
