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

#include "utilities/Reporter.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;
using namespace sptk;

/**
 * Verify that the constructor with parameters works correctly.
 */
TEST(ReporterTests, ConstructorWithParameters)
{
    const Reporter reporter("TestReporter", {"step", "time"}, Reporter::CountersFormat::CsvCounters);
    EXPECT_EQ(reporter.getOperation(), "TestReporter");
    EXPECT_EQ("step,time", reporter.getColumnNames().join(","));
    EXPECT_EQ(Reporter::CountersFormat::CsvCounters, reporter.getCountersFormat());
}

TEST(ReporterTests, PrintHeader)
{
    Reporter   reporter("TestReporter", {"step", "time"}, Reporter::CountersFormat::CsvCounters);
    const auto outputStream = reporter.printHeader();
    EXPECT_FALSE(outputStream.str().empty());
    EXPECT_TRUE(outputStream.str().find("step") != std::string::npos);
    EXPECT_TRUE(outputStream.str().find("time") != std::string::npos);
}

TEST(ReporterTests, PrintRow)
{
    Reporter         reporter("TestReporter", {"step", "time"}, Reporter::CountersFormat::CsvCounters);
    constexpr size_t totalCounter = 1000;
    constexpr size_t batchSize = 100;
    constexpr auto   latencyMs = 10.5;
    const auto       outputStream = reporter.printRow(totalCounter, batchSize, latencyMs).str();
    EXPECT_FALSE(outputStream.empty());
    EXPECT_TRUE(outputStream.find(to_string(totalCounter)) != std::string::npos);
    EXPECT_TRUE(outputStream.find(double2string(latencyMs)) != std::string::npos);
}

TEST(ReporterTests, PrintFooter)
{
    Reporter reporter("TestReporter", {"step", "time"}, Reporter::CountersFormat::CsvCounters);

    constexpr size_t totalCounter = 1000;
    constexpr size_t batchSize = 100;
    constexpr auto   latencyMs = 10.5;

    const auto str1 = reporter.printRow(totalCounter, batchSize, latencyMs).str();
    this_thread::sleep_for(100ms);
    reporter.countRow(100, 10.5);

    const auto outputStream = reporter.printFooter(false).str();
    EXPECT_FALSE(outputStream.empty());
    EXPECT_TRUE(outputStream.find("Elapsed 1") != std::string::npos);
}