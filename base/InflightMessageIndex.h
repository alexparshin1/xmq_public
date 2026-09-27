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

#include "MessageDispatch.h"
#include <sptk5/cutils>

namespace xmq {

class InflightMessageIndex final
{
public:
    /**
     * @brief Constructor
     */
    explicit InflightMessageIndex(const uint16_t maxInflightMessages = 128)
        : m_inflightMessages(maxInflightMessages < 16 ? 16 : maxInflightMessages)
    {
    }

    void resize(uint16_t maxInflightMessages)
    {
        maxInflightMessages = maxInflightMessages < 16 ? 16 : maxInflightMessages;
        m_inflightMessages.resize(maxInflightMessages);
    }

    size_t count() const
    {
        return m_messageDispatchCount;
    }

    const UMessageDispatch& get(const size_t index) const
    {
        auto& item = m_inflightMessages[index];
        if (item)
        {
            throw sptk::Exception("Attempt to use not existing id: " + std::to_string(index));
        }
        return item;
    }

    bool erase(const size_t index)
    {
        auto& item = m_inflightMessages[index];
        if (!item)
        {
            return false;
        }
        item.reset();
        --m_messageDispatchCount;
        return true;
    }

    void foreach (const std::function<void(UMessageDispatch&)>& callback)
    {
        for (auto& dispatch: m_inflightMessages)
        {
            if (dispatch)
            {
                callback(dispatch);
            }
        }
    }

private:
    size_t                        m_messageDispatchCount {0};
    std::vector<UMessageDispatch> m_inflightMessages;
};

} // namespace xmq
