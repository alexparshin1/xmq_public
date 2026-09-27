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

#include <sptk5/cutils>

namespace xmq {

class Reporter
{
public:
    enum class CountersFormat : uint8_t
    {
        NoCounters,
        CsvCounters,
        TableCounters
    };

    /**
     * @brief Constructor.
     */
    Reporter(std::string_view operation, const sptk::Strings& columnNames, CountersFormat countersFormat = CountersFormat::TableCounters);
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;
    Reporter(Reporter&&) = delete;
    Reporter& operator=(Reporter&&) = delete;
    ~Reporter() = default;

    [[nodiscard]] std::string_view getOperation() const
    {
        return m_operation;
    }

    [[nodiscard]] const sptk::Strings& getColumnNames() const
    {
        return m_columnNames;
    }

    [[nodiscard]] CountersFormat getCountersFormat() const
    {
        return m_countersFormat;
    }

    /**
     * @brief Set operation name and column names.
     * @param operation         Operation name.
     * @param columnNames       Column names.
     */
    void reset(std::string_view operation, const sptk::Strings& columnNames);

    /**
     * @brief Print the header, including the operation name and column names.
     */
    std::stringstream printHeader();

    /**
     * @brief Print the operation summary.
     * @param failedCount       Number of failed operations.
     */
    std::stringstream printFooter(bool failedCount);

    /**
     * @brief Print report row
     * @param totalCounter      Total operations counter (requested)
     * @param batchSize         Operation batch size
     * @param latencyMs         Average latency per batch
     */
    std::stringstream printRow(size_t totalCounter, size_t batchSize, double latencyMs = 0.0);

    /**
     * @brief Calculate report row
     * @param batchSize         Operation batch size
     * @param latencyMs         Average latency per batch
     */
    void countRow(size_t batchSize, double latencyMs = 0.0);

private:
    mutable std::mutex  m_mutex;                ///< Mutex that protects internal data
    std::string         m_operation;            ///< Report operation name
    sptk::Strings       m_columnNames;          ///< Report columns name
    CountersFormat      m_countersFormat;       ///< Format of the report printout
    std::atomic_size_t  m_totalCounter {0};     ///< Total operation counter (requested)
    std::vector<double> m_rowLatencyMs;         ///< Row latency, milliseconds
    std::atomic<double> m_totalLatencyMs {0.0}; ///< Total average operation latency, milliseconds
    sptk::Stopwatch     m_totalStopwatch;       ///< Total execution stopwatch
    sptk::Stopwatch     m_stepStopwatch;        ///< Step execution stopwatch
    std::atomic_size_t  m_stepCounter {0};      ///< Number of steps
};

} // namespace xmq
