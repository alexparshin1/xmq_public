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

#include <gtest/gtest.h>
#include <sptk5/threads/JoiningThread.h>
#include <sptk5/Printer.h>
#include <sptk5/Stopwatch.h>
#include <sptk5/cthreads>

using namespace std;
using namespace sptk;

namespace {
template<typename Queue>
void test2(const string_view className)
{
    Queue          queue1;
    constexpr auto iterations = 10000;

    Stopwatch sw;

    sw.start();
    sptk::JoiningThreads threads;
    constexpr auto  threadCount = 10;
    for (int t = 0; t < threadCount; ++t)
    {
        auto thread = sptk::JoiningThread(
            [&queue1]
            {
                for (int i = 0; i < iterations; ++i)
                {
                    queue1.push_back(i);
                }

                for (auto i = 0; i < iterations; ++i)
                {
                    if (auto data = 0;
                        !queue1.pop_front(data, 100ms))
                    {
                        CERR("Pop timeout at " << i);
                    }
                }
            });
        threads.push_back(std::move(thread));
    }

    for (auto& thread: threads)
    {
        thread.join();
    }

    sw.stop();

    COUT(className << ": " << setprecision(2) << fixed << static_cast<double>(iterations) / 1000.0 / sw.milliseconds() << "M/sec");
}
} // namespace

TEST(XMQ_Queue, queues)
{
    test2<SynchronizedQueue<int>>("SynchronizedQueue");
}
