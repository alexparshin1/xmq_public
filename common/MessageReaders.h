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

#include "MessageReader.h"

namespace xmq {

class XMQ_EXPORT MessageReaders final
{
public:
    /**
     * @brief Constructor
     */
    explicit MessageReaders(STopicManager topicManager);

    /**
     * @brief Destructor
     */
    ~MessageReaders() = default;

    /**
     * @brief Message reader factory
     * @param protocolVersion   MQTT protocol version
     * @return message reader
     */
    SMessageReader create(ProtocolVersion protocolVersion) const;

private:
    mutable std::shared_mutex                 m_readerMutex;
    STopicManager                             m_topicManager;
    std::map<ProtocolVersion, SMessageReader> m_readers;
};

using SMessageReaders = std::shared_ptr<MessageReaders>;

} // namespace xmq
