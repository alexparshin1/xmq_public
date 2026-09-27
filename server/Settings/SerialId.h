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

#include "base/RecordIdGenerator.h"

namespace xmq {

class SerialId
{
public:
    void setMissingIds(auto& recordSet, bool& changed)
    {
        int64_t highestId = 0;
        for (const auto& record: recordSet)
        {
            if (record.m_id.asInt64() > highestId)
            {
                highestId = record.m_id;
            }
        }

        m_recordIdGenerator.setLastRecordId(highestId);

        for (auto& record: recordSet)
        {
            if (record.m_id.isNull())
            {
                record.m_id.setInt64(m_recordIdGenerator.nextRecordId());
                changed = true;
            }
        }
    }

    uint64_t nextSerialId()
    {
        return m_recordIdGenerator.nextRecordId();
    }

private:
    RecordIdGenerator m_recordIdGenerator; ///< Next id generator
};

} // namespace xmq
