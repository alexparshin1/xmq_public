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

#include <sptk5/Buffer.h>

#include <algorithm>
#include <cstdint>

namespace xmq {

/**
 * @brief Keeps a session's read buffer about the size of what the session reads.
 *
 * A read buffer grows to the largest packet it has held, and a buffer only ever grows: one large
 * message and the session holds that much for as long as it is connected. With 200,000 sessions,
 * a kilobyte each is 200 MB. This keeps an average of the packet sizes and, when the buffer is
 * empty and well above one and a half times that average, gives it back.
 */
class ReadBufferSizer
{
public:
    /// Smallest the buffer is let down to: below it, giving memory back saves nothing.
    static constexpr uint32_t MinimumBytes = 512;

    /// A packet the session read.
    void packet(const size_t bytes)
    {
        // An exponential average, one eighth per packet: no history to keep, and a size that
        // changes for good is followed within a few dozen packets.
        const auto size = static_cast<int64_t>(std::min<size_t>(bytes, UINT32_MAX));
        const auto average = static_cast<int64_t>(m_average);
        m_average = static_cast<uint32_t>(average + (size - average) / 8);
    }

    /// The buffer has just been emptied: give back what is well above what it is used for.
    void settle(sptk::Buffer& buffer) const
    {
        if (buffer.bytes() != 0)
        {
            return;
        }
        const auto target = std::max<size_t>(MinimumBytes, m_average + m_average / 2);
        // Twice the target before it shrinks, so sizes that alternate do not reallocate each time.
        if (buffer.capacity() > 2 * target)
        {
            buffer = sptk::Buffer(target);
        }
    }

    [[nodiscard]] uint32_t average() const
    {
        return m_average;
    }

private:
    uint32_t m_average {0}; ///< Average packet size, in bytes.
};

} // namespace xmq
