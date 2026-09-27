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

#include "ServerController.h"
#include "base/xmq.h"
#include <atomic>
#include <sptk5/cutils>

namespace xmq {

class IServerController;

/**
 * OS Service
 *
 * Singleton class that starts and stops service using OS-specific methods.
 * Supports Windows, Linux, and BSD.
 */
class XMQ_EXPORT OSService
{
public:
    static IServerController* m_controlledServer; ///< Controlled server

    /**
     * @brief Constructor.
     * @param logEngine        External logger.
     * @param logPrefix        Log prefix.
     * @param daemon           The 'run as daemon' flag.
     */
    OSService(sptk::LogEngine& logEngine, const sptk::String& logPrefix, bool daemon);

    /**
     * @brief Set controlled server.
     * @param controlledServer Controlled server.
     */
    static void setServerController(IServerController* controlledServer);

    /**
     * @brief Request termination of the running service.
     *
     * Async-signal-safe: only stores to lock-free atomics. The service loop observes them and
     * performs the actual (lock-taking, thread-joining) shutdown on its own thread. Safe to call
     * from a signal handler.
     *
     * @param signal            The signal that asked for it, or 0 when no signal did. Logged by
     *                          the service loop as the reason the server stopped.
     */
    static void requestTermination(int signal = 0);

    /**
     * @brief Whether termination has been requested via requestTermination().
     */
    static bool terminationRequested();

    /**
     * @brief The signal passed to requestTermination(), or 0 if none was.
     */
    static int terminationSignal();

    /**
     * @brief Execute service.
     */
    void execute();

    /**
     * @brief Terminate service.
     */
    void terminate();

    /**
     * @brief Install service (Windows only).
     */
    void installService();

    /**
     * @brief Uninstall service (Windows only).
     */
    void uninstallService();

private:
    static std::atomic_bool s_terminationRequested; ///< Set by requestTermination(); polled by the service loop.
    static std::atomic_int  s_terminationSignal;    ///< The signal that requested termination, 0 if none.

    sptk::Logger m_logger;       ///< External logger
    bool         m_runAsService; ///< Run-as-service flag

    /**
     * @brief Service function.
     */
    void runService();
};

} // namespace xmq
