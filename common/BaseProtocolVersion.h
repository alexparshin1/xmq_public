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

#include "base/ProtocolVersion.h"
#include "base/xmq.h"

namespace xmq {

class XMQ_EXPORT BaseProtocolVersion
{
public:
    /**
     * @brief Constructor.
     */
    explicit BaseProtocolVersion(const ProtocolVersion protocolVersion)
        : m_protocolVersion(protocolVersion)
    {
    }

    [[nodiscard]] ProtocolVersion getProtocolVersion() const
    {
        return m_protocolVersion;
    }

private:
    const ProtocolVersion m_protocolVersion;
};

} // namespace xmq
