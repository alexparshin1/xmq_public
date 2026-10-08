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
    NodeDetached,
    SubscriptionSnapshot,
    SubscriptionUpdate,
    RetainedUpdate,
    ReleaseSession
};

/**
 * @brief Maps cluster commands to their MQTT control topics and back.
 *
 * Topic objects are registered with the supplied topic manager, which must outlive this instance.
 */
class Topics
{
public:
    /**
     * @brief Register MQTT control topics and initialize the command mappings.
     * @param topicManager      Manager that owns the cluster topic objects.
     */
    explicit Topics(const STopicManager& topicManager);

    /**
     * @brief Get the MQTT topic associated with a cluster command.
     * @param command           Cluster command to look up.
     * @return Topic object, or null when the command has no registered topic.
     */
    [[nodiscard]] const Topic*     getTopic(Command command) const;

    /**
     * @brief Get the full MQTT topic name associated with a cluster command.
     * @param command           Cluster command to look up.
     * @return Topic name referencing the topic object's stored name.
     * @throws sptk::Exception  When the command has no registered topic.
     */
    [[nodiscard]] std::string_view getTopicName(Command command) const;

    /**
     * @brief Identify a cluster command by its MQTT topic name.
     * @param topic             Non-null topic object to look up.
     * @return Associated command, or Command::Unknown for an unrecognized topic name.
     */
    Command                        getCommand(const Topic* topic) const;

private:
    const std::map<std::string_view, Command> m_topicCommand; ///< Maps MQTT control topic names to commands.
    const std::map<Command, const Topic*>     m_commandTopic; ///< Maps commands to topic objects owned by the topic manager.
};

} // namespace xmq::cluster
