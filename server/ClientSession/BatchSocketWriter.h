/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <sptk5/net/TCPSocket.h>

namespace xmq {

/**
 * @brief One socket's worth of output, as handed to a batch write.
 *
 * The buffer belongs to the caller and must outlive the writeAll() call - which is why writeAll()
 * is synchronous even where the underlying mechanism is not.
 */
struct SocketWriteItem
{
    /// Destination; empty items are skipped. An owning reference, not a pointer: the batch is
    /// assembled by the send worker and written afterwards, and in between a receive worker can
    /// tear down the session's connection - which destroys the socket. Holding it here is what
    /// stops the write from landing in freed memory, which AddressSanitizer caught it doing.
    sptk::STCPSocket m_socket;
    const uint8_t*   m_data {nullptr};   ///< Bytes to write.
    size_t           m_size {0};         ///< How many.
    size_t           m_written {0};      ///< Filled in by the writer.
    bool             m_failed {false};   ///< Set when the socket errored; the session is finished.

    /**
     * @brief Why it failed, empty when the peer simply went away.
     *
     * The distinction is the caller's, not the writer's: a lost connection is closed quietly,
     * anything else is logged first. Carrying the text rather than the exception keeps the batch
     * free of per-item control flow while preserving what the single-session path used to do.
     */
    std::string m_error;
};

/**
 * @brief Writes a batch of buffers, each to its own socket.
 *
 * The point of the interface is the batch: a mechanism that can submit many writes per system
 * call (io_uring on Linux, overlapped I/O on Windows) only pays off if it is given many at once.
 * The only implementation today walks the batch and writes each socket itself - an io_uring one
 * was built and measured, and gave nothing: no latency difference against this, and 2-4% of CPU,
 * which did not justify a second code path and a dependency. The receive side was the remaining
 * candidate and has since been measured too, with the same answer: a full io_uring socket pool
 * matched epoll to within 1%, because the event mechanism is 6% of the broker's syscalls while
 * thread handoffs are 67%. The interface stays for the batching itself, which is useful whether
 * or not any platform mechanism is ever put behind it.
 *
 * Implementations must not block waiting for a congested socket: in a batch that would hold up
 * every other subscriber behind one slow one. Whatever a socket does not accept is reported back
 * through SocketWriteItem::m_written for the caller to re-queue.
 */
class BatchSocketWriter
{
public:
    virtual ~BatchSocketWriter() = default;

    BatchSocketWriter() = default;
    BatchSocketWriter(const BatchSocketWriter&) = delete;
    BatchSocketWriter& operator=(const BatchSocketWriter&) = delete;

    /**
     * @brief Writes every item, filling in m_written and m_failed.
     * @param items         Batch to write; may be empty.
     */
    virtual void writeAll(std::span<SocketWriteItem> items) = 0;

    /**
     * @brief Name of the mechanism in use, for logs and for tests that assert which path ran.
     */
    [[nodiscard]] virtual std::string_view name() const = 0;
};

using UBatchSocketWriter = std::unique_ptr<BatchSocketWriter>;

/**
 * @brief Creates the best batch writer this build and machine can use.
 *
 * Today that is always the portable one. It exists now so that the send path can be shaped around
 * batches before any platform mechanism is added, and so the change that introduces io_uring
 * touches only this factory and its own implementation.
 */
UBatchSocketWriter createBatchSocketWriter();

} // namespace xmq
