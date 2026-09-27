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

#include "MessageWriter.h"

namespace xmq {

class XMQ_EXPORT MessageWriters
{
public:
    /**
     * @brief Constructor
     */
    MessageWriters();
    /**
     * @brief Destructor
     */
    virtual ~MessageWriters() = default;

    /**
     * @brief Message writer factory.
     * @param protocolVersion   Protocol version.
     * @return Message writer.
     */
    SMessageWriter create(ProtocolVersion protocolVersion) const;

private:
    mutable std::shared_mutex                 m_writersMutex;
    std::map<ProtocolVersion, SMessageWriter> m_writers;
};

using SMessageWriters = std::shared_ptr<MessageWriters>;

} // namespace xmq
