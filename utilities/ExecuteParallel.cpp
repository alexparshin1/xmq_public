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

#include "ExecuteParallel.h"
#include <sptk5/threads/JoiningThread.h>
#include "Reporter.h"

using namespace std;
using namespace sptk;

using namespace xmq;

ExecuteParallel::ExecuteParallel(Reporter& reporter, const vector<SUtilityClient>& clientList, const Function& function)
    : m_reporter(reporter)
    , m_function(function)
{
    for (auto& client: clientList)
    {
        m_clientList.push_back(client);
    }
}

size_t ExecuteParallel::execute(Reporter::CountersFormat /*showCounters*/, const size_t threadCount)
{
    JoiningThreads threads;

    m_clientCount = m_clientList.size();
    atomic_size_t totalProcessed = 0;
    atomic_size_t reportStep(m_clientCount / 20);
    if (reportStep < 1)
    {
        reportStep = 1;
    }

    mutex totalProcessedMutex;

    for (size_t i = 0; i < threadCount; ++i)
    {
        auto thread = JoiningThread(
            [this, &totalProcessed, &reportStep, &totalProcessedMutex]
            {
                while (!terminated() && !m_clientList.empty())
                {
                    if (SUtilityClient client;
                        m_clientList.pop_front(client, 100ms))
                    {
                        if (m_function(client))
                        {
                            scoped_lock lock(totalProcessedMutex);
                            if (totalProcessed == 0)
                            {
                                m_reporter.printHeader();
                            }
                            ++totalProcessed;
                            if (totalProcessed % reportStep == 0)
                            {
                                m_reporter.printRow(totalProcessed, reportStep);
                            }
                        }
                        else
                        {
                            ++m_totalFailedCount;
                        }
                    }
                }
            });
        threads.push_back(std::move(thread));
    }

    threads.clear();

    m_reporter.printFooter(m_totalFailedCount);

    return totalProcessed;
}
