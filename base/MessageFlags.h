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

#include "xmq.h"

#include "Qos.h"

namespace xmq {

class XMQ_EXPORT MessageFlags
{
    static constexpr uint8_t QOS_MASK = 0x3;
    static constexpr uint8_t DUP_SHIFT = 2;
    static constexpr uint8_t RETAIN_SHIFT = 0x3;

public:
    MessageFlags() = default;

    explicit MessageFlags(const uint8_t flags)
        : m_qos(flags & QOS_MASK)
        , m_dup(flags >> DUP_SHIFT & 0x1)
        , m_retain(flags >> RETAIN_SHIFT & 0x1)
    {
    }

    MessageFlags(Qos qos, const bool dup, const bool retain)
        : m_qos(static_cast<uint8_t>(qos))
        , m_dup(dup)
        , m_retain(retain)
    {
    }

    explicit operator uint8_t() const
    {
        return static_cast<uint8_t>(m_qos | m_dup << DUP_SHIFT | m_retain << RETAIN_SHIFT);
    }

    [[nodiscard]] Qos getQos() const
    {
        return static_cast<Qos>(m_qos);
    }

    [[nodiscard]] bool isDuplicate() const
    {
        return m_dup;
    }

    [[nodiscard]] bool isRetain() const
    {
        return m_retain;
    }

    void setQos(Qos qos)
    {
        m_qos = static_cast<uint8_t>(qos);
    }

    void setDup(const bool dup)
    {
        m_dup = dup;
    }

    void setRetain(const bool retain)
    {
        m_retain = retain;
    }

private:
    uint8_t m_qos : 2 {0};
    uint8_t m_dup : 1 {0};
    uint8_t m_retain : 1 {0};
};

} // namespace xmq
