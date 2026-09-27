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

#include <sptk5/cutils>

namespace xmq::mqtt {

class XMQ_EXPORT VariableLength final
{
public:
    /**
     * @brief Read variable length.
     * @remarks The data pointer is incremented by the number of bytes read.
     * @param data              Start of the variable length.
     * @param availableBytes
     * @return variable length value.
     */
    static uint64_t read(uint8_t*& data, int availableBytes);

    /**
     * @brief Write variable length.
     * @remarks The data pointer is incremented by the number of bytes written.
     * @param value             Variable length value.
     * @param data              Start of the variable length.
     */
    static void write(uint64_t value, uint8_t*& data);

    /**
     * @brief Get number of bytes required for storing variable length value.
     * @param value             Variable length value.
     */
    static uint8_t bytes(uint64_t value);
};

} // namespace xmq::mqtt
