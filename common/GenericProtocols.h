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

#include "GenericProtocol.h"

#include "base/ProtocolVersion.h"
#include "base/Topic.h"

namespace xmq {

class GenericProtocols
{
public:
    /**
     * @brief Constructor
     */
    explicit GenericProtocols(const STopicManager& topicManager);
    /**
     * @brief Destructor
     */
    virtual ~GenericProtocols() = default;

    /**
     * @brief Protocol factory.
     * @param protocolVersion   Protocol version.
     * @return protocol instance.
     */
    const GenericProtocol& getProtocol(ProtocolVersion protocolVersion) const;

private:
    mutable std::shared_mutex       m_mutex;
    const STopicManager             m_topicManager;
    std::array<SGenericProtocol, 3> m_protocols;
    SMessageReaders                 m_messageReaders;
    SMessageWriters                 m_messageWriters;
};

using SGenericProtocols = std::shared_ptr<GenericProtocols>;

} // namespace xmq
