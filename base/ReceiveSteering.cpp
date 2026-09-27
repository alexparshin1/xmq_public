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

#include "base/ReceiveSteering.h"

#include <filesystem>
#include <fstream>
#include <set>

#ifdef __linux__
#include <array>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

using namespace std;
using namespace xmq;

namespace fs = std::filesystem;

namespace {

string readFirstLine(const fs::path& path)
{
    ifstream file(path);
    string   line;
    if (file.is_open())
    {
        getline(file, line);
    }
    return line;
}

/// An rps_cpus mask is hex digits in comma-separated groups, one bit per CPU: "00", "70",
/// "00000000,00000000" on a big machine. Unreadable is not the same as empty, so it is not reported.
bool isEmptyMask(const string& mask)
{
    if (mask.empty())
    {
        return false;
    }
    for (const auto character: mask)
    {
        if (character != '0' && character != ',')
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool ReceiveSteering::needsSteering(const string& interfaceDirectory, const size_t usableCpus, size_t& receiveQueues)
{
    receiveQueues = 0;

    // With two CPUs there is nowhere to steer to that the interrupt's own CPU is not already
    // sharing with the broker, and RPS only adds the cost of moving packets between them.
    constexpr size_t fewestWorthSteering = 3;
    if (usableCpus < fewestWorthSteering)
    {
        return false;
    }

    const fs::path directory(interfaceDirectory);
    error_code     error;

    // Physical interfaces only. A virtual one has no card interrupt to spread.
    if (!fs::exists(directory / "device", error))
    {
        return false;
    }

    if (readFirstLine(directory / "operstate") != "up")
    {
        return false;
    }

    fs::path steeringMask;
    for (const auto& entry: fs::directory_iterator(directory / "queues", error))
    {
        if (const auto name = entry.path().filename().string(); name.starts_with("rx-"))
        {
            ++receiveQueues;
            steeringMask = entry.path() / "rps_cpus";
        }
    }

    // Several queues means the card spreads interrupts itself (RSS), and one queue that is already
    // steered has been dealt with.
    return receiveQueues == 1 && isEmptyMask(readFirstLine(steeringMask));
}

vector<string> ReceiveSteering::interfacesForAddresses(const vector<string>& bindAddresses)
{
    set<string> names;

#ifdef __linux__
    auto        everyInterface = bindAddresses.empty();
    set<string> wanted;
    for (const auto& address: bindAddresses)
    {
        if (address.empty() || address == "0.0.0.0")
        {
            everyInterface = true;
        }
        else
        {
            wanted.insert(address);
        }
    }

    ifaddrs* interfaceList = nullptr;
    if (getifaddrs(&interfaceList) != 0)
    {
        return {};
    }

    for (const auto* entry = interfaceList; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET || entry->ifa_name == nullptr)
        {
            continue;
        }

        // A secondary address can carry a label such as "eth0:1"; sysfs knows only "eth0".
        string name(entry->ifa_name);
        if (const auto colon = name.find(':'); colon != string::npos)
        {
            name.resize(colon);
        }

        if (everyInterface)
        {
            names.insert(name);
            continue;
        }

        array<char, INET_ADDRSTRLEN> buffer {};
        const auto*                  address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, buffer.data(), buffer.size()) != nullptr &&
            wanted.contains(buffer.data()))
        {
            names.insert(name);
        }
    }
    freeifaddrs(interfaceList);
#else
    (void) bindAddresses;
#endif

    return {names.begin(), names.end()};
}

vector<ReceiveSteeringGap> ReceiveSteering::findGaps(const vector<string>& bindAddresses, const size_t usableCpus,
                                                     const string& sysfsNetRoot)
{
    vector<ReceiveSteeringGap> gaps;

#ifdef __linux__
    for (const auto& name: interfacesForAddresses(bindAddresses))
    {
        if (size_t receiveQueues = 0;
            needsSteering((fs::path(sysfsNetRoot) / name).string(), usableCpus, receiveQueues))
        {
            gaps.push_back({name, receiveQueues});
        }
    }
#else
    (void) bindAddresses;
    (void) usableCpus;
    (void) sysfsNetRoot;
#endif

    return gaps;
}
