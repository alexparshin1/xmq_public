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

#include "Reporter.h"
#include "UtilityClient.h"
#include "client/MqttClient.h"

#include <sptk5/cutils>

namespace xmq {

class ExecuteParallel
{
public:
    using Function = std::function<bool(SUtilityClient&)>;

    /**
     * @brief Constructor.
     */
    ExecuteParallel(Reporter& reporter, const std::vector<SUtilityClient>& clientList, const Function& function);

    /**
     * @brief Destructor.
     */
    virtual ~ExecuteParallel()
    {
        terminate();
    }

    /**
     * @brief Execute client function in parallel.
     * @param showCounters      Print processing counters.
     * @param threadCount       Number of threads.
     */
    size_t execute(Reporter::CountersFormat showCounters, size_t threadCount);

    void terminate()
    {
        m_terminated = true;
    }

private:
    mutable std::mutex                     m_mutex;                ///< Mutex that protects internal data
    Reporter&                              m_reporter;             ///< Reporter to print statistics
    std::atomic_bool                       m_terminated {false};   ///< Is terminated flag
    sptk::SynchronizedList<SUtilityClient> m_clientList;           ///< Client list
    Function                               m_function;             ///< Processing to execute for each client in the list
    size_t                                 m_clientCount {0};      ///< Number of clients
    std::atomic_size_t                     m_totalFailedCount {0}; ///< Number of failed operations

    bool terminated() const
    {
        return m_terminated;
    }
};

} // namespace xmq
