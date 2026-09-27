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

#include "TopicAliases.h"

#include "base/ProtocolException.h"
#include "base/ProtocolVersion.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void TopicAliases::setTopicAliasMaximum(const uint16_t maxTopicAlias)
{
    m_maxTopicAlias = maxTopicAlias;
}

const Topic* TopicAliases::getTopicAlias(const uint16_t alias) const
{
    if (alias <= m_maxTopicAlias)
    {
        if (const auto iterator = m_topicAliases.find(alias);
            iterator != m_topicAliases.end())
        {
            return iterator->second;
        }
    }
    return TopicManager::emptyTopic();
}

bool TopicAliases::setTopicAlias(const uint16_t alias, const Topic* topic)
{
    if (alias == 0 || alias > m_maxTopicAlias)
    {
        return false;
    }

    if (!topic->empty())
    {
        m_topicAliases[alias] = topic;
    }
    else
    {
        m_topicAliases.erase(alias);
    }

    return true;
}

void TopicAliases::setPublishDestinationFromTopicAlias(PublishMessage* publishMessage, const uint16_t topicAlias)
{
    if (const auto destination = publishMessage->destination();
        !destination->empty())
    {
        if (setTopicAlias(topicAlias, destination))
        {
            return;
        }
    }
    else
    {
        const auto topic = getTopicAlias(topicAlias);
        if (!topic->empty())
        {
            publishMessage->setDestination(topic);
            return;
        }
    }
    stringstream str;
    str << "Topic alias " << topicAlias << " not found.";
    throw ProtocolException(ProtocolVersion::MqttV5, ReasonCode::ProtocolError, str.str());
}
