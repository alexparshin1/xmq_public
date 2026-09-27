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

#include "CpuAffinity.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

#ifdef __linux__

/**
 * @brief Parse a sysfs CPU list such as "0,4" or "0-3,8".
 */
set<int> parseCpuList(const string& text)
{
    set<int> cpus;
    size_t   position = 0;

    while (position < text.size())
    {
        auto separator = text.find(',', position);
        if (separator == string::npos)
        {
            separator = text.size();
        }

        const auto item = text.substr(position, separator - position);
        if (const auto dash = item.find('-');
            dash != string::npos)
        {
            const auto from = strtol(item.substr(0, dash).c_str(), nullptr, 10);
            const auto to = strtol(item.substr(dash + 1).c_str(), nullptr, 10);
            for (auto cpu = from; cpu <= to; ++cpu)
            {
                cpus.insert(static_cast<int>(cpu));
            }
        }
        else if (!item.empty())
        {
            cpus.insert(static_cast<int>(strtol(item.c_str(), nullptr, 10)));
        }

        position = separator + 1;
    }

    return cpus;
}

string readSysfs(const string& path)
{
    ifstream file(path);
    string   line;
    if (file.is_open())
    {
        getline(file, line);
    }
    return line;
}

/**
 * @brief CPUs this process is actually permitted to run on.
 *
 * Deliberately not /sys/devices/system/cpu/online: that is the host's view, and under a cgroup
 * cpuset (docker --cpuset-cpus, Kubernetes CPU manager) the process may be confined to a subset
 * of it. Narrowing from the host list would then pick CPUs the process cannot use, and the
 * intersection the kernel applies could leave it on fewer cores than it was granted - a silent
 * loss of capacity that still logs as success.
 */
set<int> allowedCpus()
{
    set<int>  cpus;
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) != 0)
    {
        return cpus;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &mask))
        {
            cpus.insert(cpu);
        }
    }

    return cpus;
}

/**
 * @brief One representative hardware thread per physical core, among the CPUs we may use.
 *
 * Siblings of a core all report the same thread_siblings_list, so keeping the lowest-numbered
 * member of each distinct sibling set yields exactly one thread per core. Sibling sets are
 * intersected with the permitted CPUs first, so a core whose siblings are only partly granted
 * still contributes the sibling we are allowed to run on.
 */
/**
 * @return The CPU's top speed in kHz, or 0 where the kernel does not say - which is most virtual
 *         machines, and the reason nothing here insists on having the answer.
 */
long maxFrequencyKHz(int cpu)
{
    const auto text = readSysfs("/sys/devices/system/cpu/cpu" + to_string(cpu) + "/cpufreq/cpuinfo_max_freq");
    return text.empty() ? 0 : strtol(text.c_str(), nullptr, 10);
}

/**
 * @brief Narrow a set of CPUs to the fastest ones, where a machine has more than one kind.
 *
 * Hybrid processors put slow cores beside fast ones - on a Core Ultra 9 288V the four efficiency
 * cores run at 3.7 GHz against 5.1, share one L2, and sit outside the L3 altogether. A thread that
 * lands there loses a quarter of its clock and all of its cache, and the scheduler moves threads
 * between the two kinds according to what else the machine is doing. Measured on the load client:
 * latency 540us unpinned against 507 pinned to the fast cores, and its spread 4.5% against 0.6%.
 *
 * @param usable            The CPUs this process is allowed on.
 * @param summary           Receives a phrase naming what was found, when anything was.
 * @return The fastest CPUs, or the input unchanged when they are all alike or speeds are unknown.
 */
set<int> fastestCpus(const set<int>& usable, string& summary)
{
    long fastest = 0;
    long slowest = 0;
    for (const auto cpu: usable)
    {
        const auto speed = maxFrequencyKHz(cpu);
        if (speed == 0)
        {
            return usable; // One unknown is enough: a partial picture is worse than none.
        }
        fastest = max(fastest, speed);
        slowest = slowest == 0 ? speed : min(slowest, speed);
    }

    // A few percent is the same core reported differently; a fifth is a different kind of core.
    constexpr double materialDifference = 0.05;
    if (fastest == 0 || static_cast<double>(fastest - slowest) / static_cast<double>(fastest) <= materialDifference)
    {
        return usable;
    }

    set<int> chosen;
    for (const auto cpu: usable)
    {
        if (static_cast<double>(maxFrequencyKHz(cpu)) >= static_cast<double>(fastest) * (1.0 - materialDifference))
        {
            chosen.insert(cpu);
        }
    }

    summary = "the " + to_string(chosen.size()) + " fastest cores (" + to_string(fastest / 1000) +
              " MHz against " + to_string(slowest / 1000) + ")";
    return chosen;
}

vector<int> firstSiblingPerCore(const set<int>& usable)
{

    set<set<int>> seenCores;
    vector<int>   chosen;

    for (const auto cpu: usable)
    {
        const auto siblingsText =
            readSysfs("/sys/devices/system/cpu/cpu" + to_string(cpu) + "/topology/thread_siblings_list");
        if (siblingsText.empty())
        {
            return {}; // No topology information: don't guess.
        }

        auto siblings = parseCpuList(siblingsText);

        // Keep only siblings we are permitted to use, so the choice below can never land on a
        // CPU outside the cpuset.
        set<int> permittedSiblings;
        ranges::set_intersection(siblings, usable, inserter(permittedSiblings, permittedSiblings.begin()));
        if (permittedSiblings.empty())
        {
            continue;
        }

        if (seenCores.insert(permittedSiblings).second)
        {
            chosen.push_back(*permittedSiblings.begin());
        }
    }

    return chosen;
}

/**
 * @brief Apply the affinity mask to every thread that already exists.
 *
 * Threads inherit affinity from whoever creates them, so setting it on this thread covers
 * everything the server starts later. It does not cover threads that already exist: static
 * initialisation and configuration loading bring some up before main() gets this far, and on this
 * codebase that was 18 of 72. sched_setaffinity() takes a thread id, so they can be swept directly.
 * @return number of threads whose affinity was set.
 */
size_t applyToExistingThreads(const cpu_set_t& cpuSet)
{
    error_code failure;
    size_t     applied = 0;

    for (const auto& entry: filesystem::directory_iterator("/proc/self/task", failure))
    {
        const auto name = entry.path().filename().string();
        const auto tid = strtol(name.c_str(), nullptr, 10);
        if (tid > 0 && sched_setaffinity(static_cast<pid_t>(tid), sizeof(cpuSet), &cpuSet) == 0)
        {
            ++applied;
        }
    }

    return applied;
}

#endif // __linux__

} // namespace

size_t CpuAffinity::physicalCoreCount()
{
#ifndef __linux__
    return 0;
#else
    return firstSiblingPerCore(allowedCpus()).size();
#endif
}

bool CpuAffinity::useBestCores(String& description)
{
#ifndef __linux__
    // FreeBSD has affinity of its own - cpuset(2) - but not the two things this reads to decide
    // what to ask for: the topology under /sys/devices/system/cpu and the thread list under
    // /proc/self/task. macOS has no way to pin a thread to a core at all, only a hint. Neither is
    // served by pretending.
    description = "not supported on this platform";
    return false;
#else
    const auto usable = allowedCpus();
    if (usable.empty())
    {
        description = "CPU topology unavailable, affinity unchanged";
        return false;
    }

    // Two questions, asked in this order, because they are about different things: which cores are
    // worth running on at all, and then how many threads to put on each of them. A machine can need
    // both answers, either, or neither.
    string     speedSummary;
    const auto fast = fastestCpus(usable, speedSummary);
    const auto cores = firstSiblingPerCore(fast);

    if (cores.empty())
    {
        description = "CPU topology unavailable, affinity unchanged";
        return false;
    }

    if (cores.size() >= usable.size())
    {
        description = to_string(usable.size()) + " CPU(s), all alike and one thread per core already"
                                                 ", affinity unchanged";
        return false;
    }

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    for (const auto cpu: cores)
    {
        CPU_SET(cpu, &cpuSet);
    }

    if (sched_setaffinity(0, sizeof(cpuSet), &cpuSet) != 0)
    {
        description = "affinity could not be set, unchanged";
        return false;
    }

    const auto applied = applyToExistingThreads(cpuSet);

    string cpuList;
    for (const auto cpu: cores)
    {
        cpuList += (cpuList.empty() ? "" : ",") + to_string(cpu);
    }

    // What was decided and why, because a run scored against another has to know which of these
    // happened - and on a machine where neither applies, that it was asked at all.
    string reason;
    if (!speedSummary.empty())
    {
        reason = speedSummary;
    }
    if (cores.size() < fast.size())
    {
        reason += (reason.empty() ? "" : ", ") + string("one thread per physical core");
    }

    description = to_string(cores.size()) + " of " + to_string(usable.size()) +
                  " permitted hardware threads (CPUs " + cpuList + "): " + reason + "; " +
                  to_string(applied) + " existing thread(s) updated";

    return true;
#endif
}

} // namespace xmq
