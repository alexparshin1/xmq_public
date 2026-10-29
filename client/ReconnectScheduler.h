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

#include <sptk5/net/Host.h>
#include <sptk5/net/SocketVirtualMethods.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace xmq::client {

/**
 * @brief One shared engine for reconnecting many clients at once.
 *
 * A per-client thread per reconnect does not scale: 120,000 clients coming back after a broker
 * restart needed 120,000 threads, each waking every 200 ms to check a deadline, and the load
 * client died with "Resource temporarily unavailable" before it could measure anything.
 *
 * Here a timer thread holds the deadlines, one reactor (:SocketEvents) waits for the TCP
 * connection to complete, and a small pool runs the part that still blocks - the TLS handshake,
 * the CONNECT and the wait for its answer. The reactor learns that a non-blocking connect has
 * completed from SocketEvents writability (see SocketPool::watchWrite), which is the only way to
 * know: nothing arrives to read until the client has sent CONNECT.
 */
class ReconnectScheduler final
{
public:
    /// The connection is established; the callback owns @p fd and must close it.
    using ConnectedCallback = std::function<void(SocketType fd)>;

    /// The attempt failed before a connection was established; schedule the next one.
    using FailedCallback = std::function<void()>;

    static ReconnectScheduler& instance();

    /**
     * @brief Attempt a TCP connection after @p delay.
     *
     * The callbacks run off this thread: @p onConnected on a worker, @p onFailed on the reactor.
     * An attempt that is still queued when the process shuts down is dropped.
     */
    void schedule(std::chrono::milliseconds                         delay,
                  const sptk::Host&                                 host,
                  const std::string&                                bindAddress,
                  ConnectedCallback                                 onConnected,
                  FailedCallback                                    onFailed);

    void stop();

    ReconnectScheduler(const ReconnectScheduler&) = delete;
    ReconnectScheduler& operator=(const ReconnectScheduler&) = delete;

private:
    ReconnectScheduler();
    ~ReconnectScheduler();

    struct Private;
    std::unique_ptr<Private> m_private;
};

} // namespace xmq::client
