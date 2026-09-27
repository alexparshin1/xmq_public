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

#include "HostMetrics.h"

#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#elif defined(__FreeBSD__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

using namespace std;
using namespace xmq;

namespace {

#if defined(__FreeBSD__)

/**
 * @brief Machine-wide CPU time from the kern.cp_time sysctl, in statistics-clock ticks.
 *
 * The five counters are user, nice, system, interrupt and idle. Only the ratio between two
 * readings is ever used, so the tick length does not matter here - unlike the process counter
 * below, which read() converts to milliseconds and which therefore has to be in _SC_CLK_TCK units.
 */
bool readCpTime(uint64_t& totalBusy, uint64_t& totalAll)
{
    long   cpTime[CPUSTATES] = {};
    size_t length = sizeof(cpTime);
    if (sysctlbyname("kern.cp_time", &cpTime, &length, nullptr, 0) != 0)
    {
        return false;
    }

    uint64_t sum = 0;
    for (const auto state: cpTime)
    {
        sum += static_cast<uint64_t>(state);
    }

    if (sum == 0)
    {
        return false;
    }

    const auto idle = static_cast<uint64_t>(cpTime[CP_IDLE]);
    totalAll = sum;
    totalBusy = sum > idle ? sum - idle : 0;
    return true;
}

/**
 * @brief This process's CPU time, in _SC_CLK_TCK ticks.
 *
 * FreeBSD has no /proc/self/stat to read the way Linux does, and getrusage() is the portable
 * equivalent. Its timevals are converted to the same ticks Linux reports, so that read() can go
 * on dividing by _SC_CLK_TCK without knowing which platform produced the number.
 */
bool readProcessCpuTime(uint64_t& processTime)
{
    rusage usage = {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return false;
    }

    const auto ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (ticksPerSecond <= 0)
    {
        return false;
    }

    constexpr uint64_t microsecondsPerSecond = 1000000;
    const auto         ticks = static_cast<uint64_t>(ticksPerSecond);
    const auto         toTicks = [ticks](const timeval& value)
    {
        return static_cast<uint64_t>(value.tv_sec) * ticks +
               static_cast<uint64_t>(value.tv_usec) * ticks / microsecondsPerSecond;
    };

    processTime = toTicks(usage.ru_utime) + toTicks(usage.ru_stime);
    return true;
}

/**
 * @brief Resident set size of this process, in bytes, from the kern.proc.pid sysctl.
 */
uint64_t readProcessResidentBytes()
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};

    kinfo_proc process = {};
    size_t     length = sizeof(process);
    if (sysctl(mib, 4, &process, &length, nullptr, 0) != 0 || length == 0)
    {
        return 0;
    }

    const auto pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return static_cast<uint64_t>(process.ki_rssize) * pageSize;
}

/**
 * @brief Memory from the vm.stats sysctls, in bytes.
 *
 * "Available" counts the free, inactive and laundry queues: FreeBSD's free list alone is small by
 * design on any machine that has been up for a while, and reporting it as what is left would call
 * every healthy host exhausted. Inactive and laundry pages are clean or cleanable and are what the
 * pagedaemon hands out next, which makes their sum the counterpart of Linux's MemAvailable.
 */
bool readVmStats(uint64_t& used, uint64_t& available, uint64_t& total)
{
    size_t     length = sizeof(total);
    if (sysctlbyname("hw.physmem", &total, &length, nullptr, 0) != 0 || total == 0)
    {
        return false;
    }

    const auto pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));

    const auto pageCount = [](const char* name, uint64_t& pages)
    {
        unsigned value = 0;
        size_t   size = sizeof(value);
        if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
        {
            return false;
        }
        pages = value;
        return true;
    };

    uint64_t freePages = 0;
    uint64_t inactivePages = 0;
    uint64_t laundryPages = 0;
    if (!pageCount("vm.stats.vm.v_free_count", freePages))
    {
        return false;
    }
    // Absent on an older kernel, in which case the queue it counts does not exist either.
    (void) pageCount("vm.stats.vm.v_inactive_count", inactivePages);
    (void) pageCount("vm.stats.vm.v_laundry_count", laundryPages);

    available = (freePages + inactivePages + laundryPages) * pageSize;
    if (available > total)
    {
        available = total;
    }

    // Same definition as the Linux side: everything the kernel says is not available to start new
    // work, so that the dashboard and a terminal on the same host agree.
    used = total > available ? total - available : 0;

    return true;
}

#elif !defined(_WIN32)

/**
 * @brief Machine-wide CPU time from /proc/stat, in jiffies.
 *
 * The first line totals every core. Idle time is the fourth and fifth fields (idle and iowait):
 * iowait counts as idle here, because a broker waiting on a disk is not consuming a core, and
 * counting it as busy makes a machine look saturated while it is doing nothing.
 */
bool readProcStat(uint64_t& totalBusy, uint64_t& totalAll)
{
    ifstream stat("/proc/stat");
    string   line;
    if (!stat || !getline(stat, line) || line.compare(0, 4, "cpu ") != 0)
    {
        return false;
    }

    istringstream fields(line.substr(4));
    uint64_t      value = 0;
    uint64_t      sum = 0;
    uint64_t      idle = 0;
    for (size_t index = 0; fields >> value; ++index)
    {
        sum += value;
        // user nice system idle iowait irq softirq steal guest guest_nice
        if (index == 3 || index == 4)
        {
            idle += value;
        }
    }

    if (sum == 0)
    {
        return false;
    }

    totalAll = sum;
    totalBusy = sum - idle;
    return true;
}

/**
 * @brief This process's CPU time from /proc/self/stat, in jiffies.
 *
 * Fields 14 and 15 (utime, stime) counting from one. The command name in field 2 may itself
 * contain spaces, so parsing starts after its closing parenthesis rather than splitting the
 * whole line.
 */
bool readProcSelfStat(uint64_t& processTime)
{
    ifstream stat("/proc/self/stat");
    string   line;
    if (!stat || !getline(stat, line))
    {
        return false;
    }

    const auto commandEnd = line.rfind(')');
    if (commandEnd == string::npos)
    {
        return false;
    }

    istringstream fields(line.substr(commandEnd + 1));
    string        field;
    // The field after the command name is state, which is field 3; utime is 14 and stime 15, so
    // they are the 12th and 13th values read from here.
    uint64_t utime = 0;
    uint64_t stime = 0;
    for (size_t index = 0; fields >> field; ++index)
    {
        if (index == 11)
        {
            utime = strtoull(field.c_str(), nullptr, 10);
        }
        else if (index == 12)
        {
            stime = strtoull(field.c_str(), nullptr, 10);
            processTime = utime + stime;
            return true;
        }
    }
    return false;
}

/**
 * @brief Resident set size of this process, in bytes, from /proc/self/statm.
 */
uint64_t readProcessResidentBytes()
{
    ifstream statm("/proc/self/statm");
    uint64_t totalPages = 0;
    uint64_t residentPages = 0;
    if (!statm || !(statm >> totalPages >> residentPages))
    {
        return 0;
    }
    const auto pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return residentPages * pageSize;
}

/**
 * @brief Memory from /proc/meminfo, in bytes.
 *
 * MemAvailable rather than MemFree: free memory on a busy machine is near zero because the page
 * cache has taken the rest, and reporting that as exhaustion would be alarming and wrong.
 * MemAvailable is the kernel's own estimate of what a new workload could claim.
 */
bool readMemInfo(uint64_t& used, uint64_t& available, uint64_t& total)
{
    ifstream meminfo("/proc/meminfo");
    if (!meminfo)
    {
        return false;
    }

    uint64_t free = 0;
    uint64_t buffers = 0;
    uint64_t cached = 0;
    uint64_t reclaimable = 0;

    string   name;
    uint64_t value = 0;
    string   unit;
    while (meminfo >> name >> value >> unit)
    {
        constexpr uint64_t bytesPerKb = 1024;
        const auto         bytes = value * bytesPerKb;
        if (name == "MemTotal:")
        {
            total = bytes;
        }
        else if (name == "MemAvailable:")
        {
            available = bytes;
        }
        else if (name == "MemFree:")
        {
            free = bytes;
        }
        else if (name == "Buffers:")
        {
            buffers = bytes;
        }
        else if (name == "Cached:")
        {
            cached = bytes;
        }
        else if (name == "SReclaimable:")
        {
            reclaimable = bytes;
        }
    }

    // Memory in use as free(1) and top report it: everything the kernel says is not available to
    // start new work. Current procps defines its "used" column exactly this way, so the dashboard
    // and a terminal on the same host agree. The older definition - total less free less cache -
    // is deliberately not used: it ignores cache that cannot be reclaimed, such as a large tmpfs,
    // and so understates use on the machines where it matters most.
    (void) free;
    (void) buffers;
    (void) cached;
    (void) reclaimable;
    used = total > available ? total - available : 0;

    return total != 0;
}

#endif // platform

} // namespace

bool HostMetrics::readCpuCounters(uint64_t& totalBusy, uint64_t& totalAll, uint64_t& processTime)
{
#ifdef _WIN32
    // Untested on Windows at the time of writing - built and verified on Linux only.
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime) == 0)
    {
        return false;
    }

    const auto toUint64 = [](const FILETIME& fileTime)
    {
        ULARGE_INTEGER value;
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return static_cast<uint64_t>(value.QuadPart);
    };

    // Kernel time includes idle time, so the machine's total is kernel + user and its busy part
    // is that total less idle.
    const auto idle = toUint64(idleTime);
    totalAll = toUint64(kernelTime) + toUint64(userTime);
    totalBusy = totalAll > idle ? totalAll - idle : 0;

    FILETIME creationTime;
    FILETIME exitTime;
    FILETIME processKernelTime;
    FILETIME processUserTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &processKernelTime, &processUserTime) == 0)
    {
        return false;
    }
    processTime = toUint64(processKernelTime) + toUint64(processUserTime);

    return totalAll != 0;
#elif defined(__FreeBSD__)
    if (!readCpTime(totalBusy, totalAll))
    {
        return false;
    }
    return readProcessCpuTime(processTime);
#else
    if (!readProcStat(totalBusy, totalAll))
    {
        return false;
    }
    return readProcSelfStat(processTime);
#endif
}

bool HostMetrics::readMemory(uint64_t& usedByProcess, uint64_t& used, uint64_t& available, uint64_t& total)
{
#ifdef _WIN32
    // Untested on Windows at the time of writing - built and verified on Linux only.
    MEMORYSTATUSEX memoryStatus;
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) == 0)
    {
        return false;
    }
    total = static_cast<uint64_t>(memoryStatus.ullTotalPhys);
    available = static_cast<uint64_t>(memoryStatus.ullAvailPhys);
    // Windows has no equally cheap split between cache and use, so the two figures coincide here.
    used = total > available ? total - available : 0;

    PROCESS_MEMORY_COUNTERS processMemory;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &processMemory, sizeof(processMemory)) != 0)
    {
        // The closest equivalent of resident set size.
        usedByProcess = static_cast<uint64_t>(processMemory.WorkingSetSize);
    }
    return true;
#elif defined(__FreeBSD__)
    usedByProcess = readProcessResidentBytes();
    return readVmStats(used, available, total);
#else
    usedByProcess = readProcessResidentBytes();
    return readMemInfo(used, available, total);
#endif
}

bool HostMetrics::readDiskSpace(const filesystem::path& directory, uint64_t& available, uint64_t& total)
{
    if (directory.empty())
    {
        return false;
    }

    // std::filesystem reports the same numbers on both platforms, so this needs no split - and it
    // reports failure as an error code rather than by throwing, which suits a metric that must
    // never take the dashboard down.
    error_code errorCode;
    const auto space = filesystem::space(directory, errorCode);
    if (errorCode)
    {
        return false;
    }

    // "available" rather than "free": the two differ by the reserve only root may use, and a
    // server that is not root cannot have it.
    available = static_cast<uint64_t>(space.available);
    total = static_cast<uint64_t>(space.capacity);
    return total != 0;
}

HostMetricsSnapshot HostMetrics::read()
{
    const scoped_lock lock(m_mutex);

    HostMetricsSnapshot snapshot;

    uint64_t   totalBusy = 0;
    uint64_t   totalAll = 0;
    uint64_t   processTime = 0;
    const auto now = chrono::steady_clock::now();

    if (readCpuCounters(totalBusy, totalAll, processTime))
    {
        snapshot.m_available = true;

        if (m_hasPreviousSample)
        {
            if (const auto allDelta = totalAll - m_previousTotalAll;
                totalAll > m_previousTotalAll)
            {
                const auto busyDelta = totalBusy > m_previousTotalBusy ? totalBusy - m_previousTotalBusy : 0;
                snapshot.m_cpuPercentTotal = 100.0 * static_cast<double>(busyDelta) / static_cast<double>(allDelta);

                // The process is expressed against elapsed time rather than against the machine's
                // total, so a busy broker on an idle 16-core box reads as 400%, not 25% - the same
                // number top would show, and the one that says how many cores it is using.
                const auto processDelta = processTime > m_previousProcess ? processTime - m_previousProcess : 0;
                if (const auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - m_previousSampleTime).count();
                    elapsed > 0)
                {
#ifdef _WIN32
                    // FILETIME units are 100ns.
                    const auto processMilliseconds = static_cast<double>(processDelta) / 10000.0;
#else
                    const auto ticksPerSecond = static_cast<double>(sysconf(_SC_CLK_TCK));
                    const auto processMilliseconds = ticksPerSecond > 0
                                                         ? static_cast<double>(processDelta) * 1000.0 / ticksPerSecond
                                                         : 0.0;
#endif
                    snapshot.m_cpuPercentProcess = 100.0 * processMilliseconds / static_cast<double>(elapsed);
                }
            }
        }

        m_previousTotalBusy = totalBusy;
        m_previousTotalAll = totalAll;
        m_previousProcess = processTime;
        m_previousSampleTime = now;
        m_hasPreviousSample = true;
    }

    if (readMemory(snapshot.m_memoryUsedByProcess, snapshot.m_memoryUsed, snapshot.m_memoryAvailable, snapshot.m_memoryTotal))
    {
        snapshot.m_available = true;
    }

    // Needed to put the process figure in scale: on its own, "400%" says nothing about whether
    // that is most of the machine or a quarter of it.
#ifdef _WIN32
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    snapshot.m_cpuCores = static_cast<uint32_t>(systemInfo.dwNumberOfProcessors);
#else
    if (const auto cores = sysconf(_SC_NPROCESSORS_ONLN);
        cores > 0)
    {
        snapshot.m_cpuCores = static_cast<uint32_t>(cores);
    }
#endif

    snapshot.m_processUptimeSeconds =
        static_cast<uint64_t>(chrono::duration_cast<chrono::seconds>(now - m_startedAt).count());

    return snapshot;
}
