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

#include <base/Topic.h>
#include <common/PublishMessage.h>

namespace xmq {

/**
 * @brief MQTT5-specific topic aliases.
 */
class TopicAliases
{
public:
    /**
     * @brief Set maximum topic alias.
     * @param maxTopicAlias     Maximum topic alias.
     */
    void setTopicAliasMaximum(uint16_t maxTopicAlias);

    /**
     * @brief Get topic by its alias.
     * @param alias Topic.
     * @return topic.
     */
    [[nodiscard]] const Topic* getTopicAlias(uint16_t alias) const;

    /**
     * @brief Set topic alias.
     * @param alias Topic alias
     * @param topic Topic name
     * @return true if topic alias is set
     */
    bool setTopicAlias(uint16_t alias, const Topic* topic);

    /**
     * @brief Set publish destination from topic alias
     * @param publishMessage Publish message
     * @param topicAlias Topic alias
     */
    void setPublishDestinationFromTopicAlias(PublishMessage* publishMessage, uint16_t topicAlias);

private:
    uint16_t                         m_maxTopicAlias {0}; ///< Maximum topic alias
    std::map<uint16_t, const Topic*> m_topicAliases;      ///< Topic aliases
};

} // namespace xmq
