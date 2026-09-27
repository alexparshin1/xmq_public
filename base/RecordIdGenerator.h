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

#include "base/xmq.h"

namespace xmq {

/**
 * Next record id generator
 */
class XMQ_EXPORT RecordIdGenerator
{
public:
    /**
     * @brief Default constructor.
     */
    RecordIdGenerator() = default;

    /**
     * @brief Copy constructor.
     * @param other Another object.
     */
    RecordIdGenerator(const RecordIdGenerator& other)
        : m_lastRecordId(other.m_lastRecordId)
    {
    }

    /**
     * @brief Destructor.
     */
    ~RecordIdGenerator() = default;

    /**
     * @brief Set last record id.
     * @param id                Last record id.
     */
    void setLastRecordId(const uint64_t id)
    {
        std::scoped_lock lock(m_mutex);
        m_lastRecordId = id;
    }

    /**
     * @brief Get next record id.
     * @return Next record id.
     */
    uint64_t nextRecordId()
    {
        std::scoped_lock lock(m_mutex);
        ++m_lastRecordId;
        return m_lastRecordId;
    }

private:
    std::mutex m_mutex;            ///< Mutex for thread-safe record id generation
    uint64_t   m_lastRecordId {0}; ///< Last record id
};

} // namespace xmq
