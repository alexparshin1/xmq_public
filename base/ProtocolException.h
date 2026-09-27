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

#include "ProtocolVersion.h"
#include "ReasonCode.h"
#include <sptk5/Exception.h>


namespace xmq {

/**
 * @brief Protocol-related exception.
 */
class XMQ_EXPORT ProtocolException final : public sptk::Exception
{
public:
    /**
     * @brief Constructor.
     * @param protocolVersion   Current protocol version.
     * @param reasonCode        Reason code.
     * @param text              Reason description.
     * @param location          The code location where it's thrown from.
     */
    ProtocolException(const ProtocolVersion protocolVersion, const ReasonCode reasonCode, const sptk::String& text, const std::source_location& location = std::source_location::current())
        : Exception(text, location)
        , m_protocolVersion(protocolVersion)
        , m_reasonCode(reasonCode)
    {
    }

    /**
     * @return Protocol version
     */
    [[nodiscard]] ProtocolVersion getProtocolVersion() const
    {
        return m_protocolVersion;
    }

    /**
     * @return Reason code
     */
    [[nodiscard]] ReasonCode getReasonCode() const
    {
        return m_reasonCode;
    }

private:
    ProtocolVersion m_protocolVersion;
    ReasonCode      m_reasonCode;
};

} // namespace xmq
