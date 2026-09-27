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
// This test is comparing the performance of std::unordered_map and gtl::flat_hash_map from https://github.com/greg7mdp/gtl.

#include <format>
#include <gtest/gtest.h>
#include <gtl/phmap.hpp>
#include <sptk5/Printer.h>
#include <sptk5/Stopwatch.h>

using namespace std;
using gtl::flat_hash_map;

TEST(Maps, Compare)
{
    unordered_map<string, size_t> testData;

    constexpr size_t iterations = 250000;
    for (size_t i = 0; i < iterations; ++i)
    {
        testData[format("topic/{}", i)] = i;
    }

    sptk::Stopwatch stopwatch;

    {
        map<string, size_t> map;
        stopwatch.start();
        for (const auto& [key, value]: testData)
        {
            map[key] = value;
        }
        stopwatch.stop();
        auto addMs = stopwatch.milliseconds();

        stopwatch.start();
        for (size_t i = 0; i < iterations; ++i)
        {
            (void) map.find(format("topic/{}", i));
        }
        stopwatch.stop();

        COUT(format("{:<15s} add {:8.2f}ms read {:8.2f}ms", "map", addMs, stopwatch.milliseconds()));
    }

    {
        unordered_map<string, size_t> map;
        stopwatch.start();
        for (const auto& [key, value]: testData)
        {
            map[key] = value;
        }
        stopwatch.stop();
        auto addMs = stopwatch.milliseconds();

        stopwatch.start();
        for (size_t i = 0; i < iterations; ++i)
        {
            (void) map.find(format("topic/{}", i));
        }
        stopwatch.stop();
        COUT(format("{:<15s} add {:8.2f}ms read {:8.2f}ms", "unordered_map", addMs, stopwatch.milliseconds()));
    }

    {
        flat_hash_map<string, size_t> map;
        stopwatch.start();
        for (const auto& [key, value]: testData)
        {
            map[key] = value;
        }
        stopwatch.stop();
        auto addMs = stopwatch.milliseconds();

        stopwatch.start();
        for (size_t i = 0; i < iterations; ++i)
        {
            (void) map.find(format("topic/{}", i));
        }
        stopwatch.stop();
        COUT(format("{:<15s} add {:8.2f}ms read {:8.2f}ms", "flat_hash_map", addMs, stopwatch.milliseconds()));
    }
}
