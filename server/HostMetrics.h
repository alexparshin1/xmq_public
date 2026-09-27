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

#include "base/xmq.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace xmq {

/**
 * @brief What the machine and this process are using, as the dashboard shows it.
 *
 * Every field is a snapshot taken at the same moment, so the numbers on screen agree with each
 * other. A field that could not be read is left at zero and reported as unavailable rather than
 * guessed - a dash on the dashboard says more than a plausible wrong number.
 */
struct HostMetricsSnapshot
{
    bool     m_available {false};        ///< False if this platform could not be read at all.

    double   m_cpuPercentTotal {0};      ///< Whole machine, percent of all cores together (0..100).
    double   m_cpuPercentProcess {0};    ///< This process, percent of one core (may exceed 100).
    uint32_t m_cpuCores {0};             ///< Hardware threads, so a process figure can be put in scale.

    uint64_t m_memoryUsedByProcess {0};  ///< Resident set size of this process, bytes.
    uint64_t m_memoryUsed {0};           ///< Memory in use, excluding reclaimable cache, bytes.
    uint64_t m_memoryAvailable {0};      ///< Memory available to start new work, bytes.
    uint64_t m_memoryTotal {0};          ///< Physical memory installed, bytes.

    uint64_t m_processUptimeSeconds {0}; ///< How long this process has been running.
};

/**
 * @brief Reads CPU, memory and disk use of the host and of this process.
 *
 * CPU is the reason this is a class rather than a function: a processor time counter only means
 * something as a difference between two readings, so the first call establishes a baseline and
 * every call after it reports the interval since the one before. Callers get a figure that is
 * already a percentage, identical for everyone reading it, instead of each having to remember a
 * previous sample of its own.
 *
 * Implemented per platform - /proc and statvfs on Linux, the sysctl tree on FreeBSD, the
 * performance and memory APIs on Windows. Anything that cannot be read leaves its field at zero.
 */
class XMQ_EXPORT HostMetrics
{
public:
    HostMetrics() = default;

    /**
     * @brief Free and total space of the filesystem holding a directory, in bytes.
     *
     * Separate from read(), because which directory is worth reporting is the caller's business:
     * disk only means something once you know whose disk it is.
     *
     * @param directory Directory to measure.
     * @param available Receives space available to this user.
     * @param total     Receives the size of the filesystem.
     * @return true if both could be read.
     */
    static bool readDiskSpace(const std::filesystem::path& directory, uint64_t& available, uint64_t& total);

    /**
     * @brief Read the current values.
     *
     * CPU percentages cover the time since the previous call, so calling this on a regular timer
     * gives a stable reading; the very first call has no interval behind it and reports zero.
     *
     * @return the snapshot.
     */
    [[nodiscard]] HostMetricsSnapshot read();

private:
    mutable std::mutex m_mutex;

    // Previous CPU counters, kept so the next read can express the difference as a percentage.
    bool     m_hasPreviousSample {false};
    uint64_t m_previousTotalBusy {0};  ///< Machine-wide busy time, platform units.
    uint64_t m_previousTotalAll {0};   ///< Machine-wide total time, platform units.
    uint64_t m_previousProcess {0};    ///< This process's CPU time, platform units.
    std::chrono::steady_clock::time_point m_previousSampleTime;

    std::chrono::steady_clock::time_point m_startedAt {std::chrono::steady_clock::now()};

    /**
     * @brief Fill in the platform's raw CPU counters.
     * @param totalBusy     Receives machine-wide busy time.
     * @param totalAll      Receives machine-wide total time, busy and idle.
     * @param processTime   Receives this process's CPU time, in the same units.
     * @return true if all three could be read.
     */
    static bool readCpuCounters(uint64_t& totalBusy, uint64_t& totalAll, uint64_t& processTime);

    /**
     * @brief Fill in the platform's memory figures, in bytes.
     * @return true if they could be read.
     */
    static bool readMemory(uint64_t& usedByProcess, uint64_t& used, uint64_t& available, uint64_t& total);

};

} // namespace xmq
