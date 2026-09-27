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

#include "SerialId.h"
#include "service/CListener.h"
#include <sptk5/cutils>

namespace xmq {

class Settings;

class XMQ_EXPORT ListenerManager final : SerialId
{
public:
    using OnChangeEvent = std::function<void()>;

    /**
     * @brief Constructor.
     */
    ListenerManager(Settings* settings, const OnChangeEvent& onChange);

    /**
     * @brief Constructor.
     */
    ListenerManager(const ListenerManager& onChange) = default;

    /**
     * @brief Destructor.
     */
    ~ListenerManager() = default;

    void     initialize(bool& changed);
    uint64_t control(const std::string& action, const CListener& listener);

private:
    Settings*     m_settings {nullptr};
    OnChangeEvent m_onChange;
};

} // namespace xmq
