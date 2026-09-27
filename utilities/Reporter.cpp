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

#include "Reporter.h"
#include <cmath>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {
ostream& operator<<(std::ostream& stream, const Reporter::CountersFormat format)
{
    if (format == Reporter::CountersFormat::CsvCounters)
    {
        stream << ",";
    }
    else
    {
        stream << setw(15);
    }
    return stream;
}
} // namespace

Reporter::Reporter(const std::string_view operation, const Strings& columnNames, const CountersFormat countersFormat)
    : m_operation(operation)
    , m_columnNames(columnNames)
    , m_countersFormat(countersFormat)
{
}

void Reporter::reset(const std::string_view operation, const Strings& columnNames)
{
    m_operation = operation;
    m_columnNames = columnNames;
    m_totalStopwatch.start();
    m_stepStopwatch.start();
}

stringstream Reporter::printHeader()
{
    scoped_lock lock(m_mutex);

    stringstream outputStream;

    if (m_countersFormat == CountersFormat::NoCounters)
    {
        return outputStream;
    }

    if (m_countersFormat == CountersFormat::TableCounters)
    {
        outputStream << setw(15) << right;
    }

    outputStream << m_operation << ":\n";

    auto first = true;
    for (const auto& columnName: m_columnNames)
    {
        if (first)
        {
            first = false;
            outputStream << columnName;
        }
        else
        {
            outputStream << m_countersFormat << columnName;
        }
    }
    COUT("\n"
         << outputStream.str());

    m_totalStopwatch.start();
    m_stepStopwatch.start();
    m_stepCounter = 0;

    return outputStream;
}

stringstream Reporter::printRow(const size_t totalCounter, const size_t batchSize, const double latencyMs)
{
    scoped_lock lock(m_mutex);

    m_stepStopwatch.stop();

    stringstream outputStream;

    if (m_countersFormat != CountersFormat::NoCounters)
    {
        if (m_countersFormat == CountersFormat::TableCounters)
        {
            outputStream << setw(15);
        }

        if (latencyMs < 1E-4)
        {
            outputStream << totalCounter << m_countersFormat << fixed << setprecision(2) << m_stepStopwatch.milliseconds()
                         << m_countersFormat << setprecision(1) << static_cast<double>(batchSize) / m_stepStopwatch.milliseconds();
        }
        else
        {
            outputStream << totalCounter << m_countersFormat << fixed << setprecision(2) << m_stepStopwatch.milliseconds()
                         << m_countersFormat << setprecision(1) << static_cast<double>(batchSize) / m_stepStopwatch.milliseconds()
                         << m_countersFormat << latencyMs;
            m_totalLatencyMs += latencyMs;
            m_rowLatencyMs.push_back(latencyMs);
        }

        ++m_stepCounter;

        COUT(outputStream.str());
    }

    m_totalCounter += batchSize;

    m_stepStopwatch.start();

    return outputStream;
}

void Reporter::countRow(const size_t batchSize, const double latencyMs)
{
    scoped_lock lock(m_mutex);

    m_stepStopwatch.stop();

    if (latencyMs >= 1E-4)
    {
        m_totalLatencyMs += latencyMs;
        m_rowLatencyMs.push_back(latencyMs);
    }

    ++m_stepCounter;
    m_totalCounter += batchSize;
    m_stepStopwatch.start();
}

stringstream Reporter::printFooter(const bool failedCount)
{
    stringstream footer;
    m_totalStopwatch.stop();

    if (m_totalStopwatch.seconds() < 1)
    {
        footer << "\n"
               << m_operation << ": Elapsed " << fixed << setprecision(2) << m_totalStopwatch.milliseconds() << "ms";
    }
    else
    {
        footer << "\n"
               << m_operation << ": Elapsed " << fixed << setprecision(2) << m_totalStopwatch.seconds() << "sec";
    }

    if (failedCount)
    {
        footer << ", failed " << failedCount;
    }

    if (const auto rate = static_cast<double>(m_totalCounter) / m_totalStopwatch.seconds();
        rate < 1000)
    {
        footer << ", the rate is " << static_cast<double>(m_totalCounter) / m_totalStopwatch.seconds() << "/sec. ";
    }
    else
    {
        footer << ", the rate is " << static_cast<double>(m_totalCounter) / m_totalStopwatch.milliseconds() << "K/sec. ";
    }

    if (m_totalLatencyMs > 1E-6)
    {
        const auto averageLatencyMs = m_totalLatencyMs / static_cast<double>(m_stepCounter);
        if (averageLatencyMs < 1000)
        {
            footer << "Average latency is " << averageLatencyMs << " ms";
        }
        else
        {
            footer << "Average latency is " << averageLatencyMs / 1000.0 << " sec.";
        }
    }

    COUT(footer.str());

    return footer;
}

} // namespace xmq
