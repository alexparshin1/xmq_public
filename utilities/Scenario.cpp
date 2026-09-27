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

#include <array>
#include <algorithm>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "base/ReceiveSteering.h"
#include "Scenario.h"
#include "ScenarioCommandLine.h"
#include "base/CpuAffinity.h"
#include "scenario/ScenarioEngine.h"

#ifdef _WIN32
// Keep these four in separate blocks: the include sorter only sorts within a contiguous run, and
// the required order is not the alphabetical one. winsock2.h must precede windows.h, or windows.h
// pulls in winsock.h (v1) and winsock2.h then fails with "WinSock.h has already been included" -
// and alphabetically windows.h sorts first. iphlpapi.h needs the winsock2 socket types, so it
// follows both. Do not merge these blocks.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include <sptk5/net/Socket.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#endif
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

filesystem::path executableDirectory(const filesystem::path& argv0)
{
#ifdef _WIN32
    // The wide variant, because filesystem::path is natively wchar_t on Windows: the ANSI one
    // would round-trip through the active code page and mangle any non-ASCII directory name.
    //
    // GetModuleFileNameW returns the character count excluding the terminator, or exactly the
    // buffer size when the name did not fit, so the buffer grows until a call comes back short.
    // MAX_PATH is only a starting guess - it stopped being a ceiling once long paths were enabled.
    constexpr size_t maxWindowsPath = 32768;

    for (vector<wchar_t> buffer(MAX_PATH);;)
    {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || buffer.size() >= maxWindowsPath)
        {
            break; // Failed outright, or grew past any legal path length.
        }
        if (length < buffer.size())
        {
            return filesystem::path(buffer.data()).parent_path(); // Null-terminated on success.
        }
        buffer.resize(buffer.size() * 2);
    }

    error_code errorCode;
    return filesystem::absolute(argv0, errorCode).parent_path();
#else
    error_code errorCode;
    auto       executablePath = filesystem::read_symlink("/proc/self/exe", errorCode);
    if (errorCode)
    {
        executablePath = filesystem::absolute(argv0, errorCode);
    }
    return executablePath.parent_path();
#endif
}

Strings matchingInterfaceAddresses(const String& interfaceMask)
{
    const Strings parts(interfaceMask, "/");
#ifdef _WIN32
    // InetPtonA is a Winsock call and fails with WSANOTINITIALISED unless Winsock has been started.
    // SPTK starts it lazily from Host's constructor, which has not necessarily run by the time
    // command line options are resolved. Socket::init() is guarded, so this is a no-op afterwards.
    Socket::init();

    in_addr baseAddress {};
    if (parts.size() != 2 || InetPtonA(AF_INET, parts[0].c_str(), &baseAddress) != 1)
    {
        throw Exception("Invalid interface mask '" + interfaceMask + "'. Expected <IPv4 address>/<prefix length>.");
    }

    constexpr auto maxPrefixLength = 32;
    const auto     prefixLength = string2int(parts[1], -1);
    if (prefixLength < 0 || prefixLength > maxPrefixLength)
    {
        throw Exception(format("Invalid prefix length in interface mask '{}'. Expected 0..{}.", interfaceMask.c_str(), maxPrefixLength));
    }
    const auto networkMask = prefixLength == 0 ? 0 : htonl(~static_cast<uint32_t>(0) << (maxPrefixLength - prefixLength));


    ULONG                family = AF_INET;
    ULONG                flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG                bufferSize = 15000;
    std::vector<uint8_t> buffer;
    buffer.resize(bufferSize);

    PIP_ADAPTER_ADDRESSES adapterAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG                 ret = GetAdaptersAddresses(family, flags, nullptr, adapterAddresses, &bufferSize);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        // Resize buffer and retry
        buffer.resize(bufferSize);
        adapterAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(family, flags, nullptr, adapterAddresses, &bufferSize);
    }

    if (ret != NO_ERROR)
    {
        throw Exception("Can't enumerate local network interfaces");
    }

    set<String> uniqueAddresses;
    for (PIP_ADAPTER_ADDRESSES adapter = adapterAddresses; adapter != nullptr; adapter = adapter->Next)
    {
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next)
        {
            if (ua->Address.lpSockaddr == nullptr || ua->Address.lpSockaddr->sa_family != AF_INET)
            {
                continue;
            }
            const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);
            const in_addr      address = sin->sin_addr;
            if ((address.S_un.S_addr & networkMask) == (baseAddress.S_un.S_addr & networkMask))
            {
                char buff[INET_ADDRSTRLEN] = {};
                if (InetNtopA(AF_INET, &address, buff, sizeof(buff)) != nullptr)
                {
                    uniqueAddresses.emplace(buff);
                }
            }
        }
    }

    // No free() here: adapterAddresses points into 'buffer', which was allocated by the vector via
    // operator new and is released by its destructor. Calling free() on it would be a double free.

    Strings addresses;
    for (const auto& address: uniqueAddresses)
    {
        addresses.push_back(address);
    }
    return addresses;
#else
    in_addr baseAddress {};
    if (parts.size() != 2 || inet_pton(AF_INET, parts[0].c_str(), &baseAddress) != 1)
    {
        throw Exception("Invalid interface mask '" + interfaceMask + "'. Expected <IPv4 address>/<prefix length>.");
    }

    constexpr auto maxPrefixLength = 32;
    const auto     prefixLength = string2int(parts[1], -1);
    if (prefixLength < 0 || prefixLength > maxPrefixLength)
    {
        throw Exception(format("Invalid prefix length in interface mask '{}'. Expected 0..{}.", interfaceMask.c_str(), maxPrefixLength));
    }
    const auto networkMask = prefixLength == 0 ? 0 : htonl(~static_cast<uint32_t>(0) << (maxPrefixLength - prefixLength));

    ifaddrs* interfaceList = nullptr;
    if (getifaddrs(&interfaceList) != 0)
    {
        throw Exception("Can't enumerate local network interfaces: "s + strerror(errno));
    }

    set<String> uniqueAddresses;
    for (const auto* iface = interfaceList; iface != nullptr; iface = iface->ifa_next)
    {
        if (iface->ifa_addr == nullptr || iface->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        const auto address = reinterpret_cast<const sockaddr_in*>(iface->ifa_addr)->sin_addr;
        if ((address.s_addr & networkMask) == (baseAddress.s_addr & networkMask))
        {
            array<char, INET_ADDRSTRLEN> buffer {};
            if (inet_ntop(AF_INET, &address, buffer.data(), buffer.size()) != nullptr)
            {
                uniqueAddresses.emplace(buffer.data());
            }
        }
    }
    freeifaddrs(interfaceList);

    Strings addresses;
    for (const auto& address: uniqueAddresses)
    {
        addresses.push_back(address);
    }
    return addresses;
#endif
}

} // namespace

filesystem::path Scenario::resolveScenarioFile(const filesystem::path& scenarioFile, const filesystem::path& executableDirectory)
{
    if (scenarioFile.is_absolute())
    {
        return scenarioFile;
    }

    // Use the non-throwing overload: a filesystem error here (permission denied on an
    // intermediate directory, ELOOP, ...) must fall through to the next lookup location
    // instead of escaping as std::filesystem::filesystem_error, which isn't an
    // sptk::Exception and so wouldn't be caught by main()'s exception handler.

    if (error_code cwdError;
        filesystem::exists(scenarioFile, cwdError))
    {
        return scenarioFile;
    }

    const auto installedScenarioDirectory = executableDirectory / ".." / "share" / "xmq";
    error_code installedError;
    if (const auto installedScenarioFile = installedScenarioDirectory / scenarioFile;
        filesystem::exists(installedScenarioFile, installedError))
    {
        return installedScenarioFile;
    }

    throw Exception("Scenario file '" + scenarioFile.string() + "' is not found in the current directory or in '" +
                    installedScenarioDirectory.lexically_normal().string() + "'.");
}

map<string, vector<string>, less<>> Scenario::listScenarios(const filesystem::path& executableDirectory)
{
    map<string, vector<string>, less<>> groups;

    const auto installedScenarioDirectory = executableDirectory / ".." / "share" / "xmq";
    if (error_code errorCode;
        !filesystem::is_directory(installedScenarioDirectory, errorCode))
    {
        return groups;
    }

    for (const auto& groupEntry: filesystem::directory_iterator(installedScenarioDirectory))
    {
        if (!groupEntry.is_directory())
        {
            continue;
        }

        const auto groupName = groupEntry.path().filename().string();

        vector<string> scenarioFiles;
        for (const auto& fileEntry: filesystem::directory_iterator(groupEntry.path()))
        {
            if (fileEntry.is_regular_file() && fileEntry.path().extension() == ".json")
            {
                scenarioFiles.push_back((filesystem::path(groupName) / fileEntry.path().filename()).string());
            }
        }
        ranges::sort(scenarioFiles);

        groups.try_emplace(groupName, std::move(scenarioFiles));
    }

    return groups;
}

void Scenario::printScenarioList() const
{
    const auto groups = listScenarios(m_executableDirectory);

    if (groups.empty())
    {
        COUT("No scenario files found.");
        return;
    }

    for (const auto& [groupName, scenarioFiles]: groups)
    {
        COUT(groupName << ":");
        for (const auto& scenarioFile: scenarioFiles)
        {
            COUT("  " << scenarioFile);
        }
    }
}

Strings Scenario::resolveBindInterfaces(const String& definition)
{
    Strings addresses;
    if (definition.contains("/"))
    {
        addresses = matchingInterfaceAddresses(definition);
        if (addresses.empty())
        {
            throw Exception("No local interface addresses match '" + definition + "'.");
        }
    }
    else
    {
        for (const auto& address: Strings(definition, ","))
        {
            if (in_addr parsedAddress {};
                inet_pton(AF_INET, address.c_str(), &parsedAddress) != 1)
            {
                throw Exception("Invalid IP address '" + address + "' in the interface list.");
            }
            addresses.push_back(address);
        }
    }
    return addresses;
}

Scenario::Scenario(const vector<string>& args)
    : Utility(make_shared<ScenarioCommandLine>(args))
    , m_executableDirectory(executableDirectory(args.empty() ? filesystem::path() : filesystem::path(args.front())))
{
}

int Scenario::run()
{
    if (commandLine().hasOption("list-scenarios"))
    {
        printScenarioList();
        return 0;
    }

#ifdef __linux__
    String description;
    bool   applied = false;
    if (commandLine().hasOption("use-cpu-affinity"))
    {
        // The same call the broker makes for itself. Said out loud either way: a run scored against
        // an earlier one has to know whether the client was pinned, and this line is the record.
        applied = CpuAffinity::useBestCores(description);
    }
    else
    {
        description = "disabled by default; use --use-cpu-affinity to enable";
    }
    COUT("CPU affinity: " << (applied ? "" : "not applied - ") << description.c_str() << std::endl);
#endif

    // The scenario engine creates, connects, and disconnects its own client groups
    // as defined in the scenario file.
    executeScenario();

    return 0;
}

namespace {

/// Interfaces that carry a tunnel rather than a wire. A load test whose traffic goes through one is
/// measuring the tunnel: on 2026-09-20 a NordVPN connection held the default route on the bench
/// client while the bench read a released build at two and a half times its own recorded latency.
bool looksLikeTunnel(const std::string& interfaceName)
{
    static const std::array prefixes {"tun", "tap", "wg", "nordlynx", "ppp", "utun"};
    return std::ranges::any_of(prefixes, [&interfaceName](const char* prefix)
                               { return interfaceName.starts_with(prefix); });
}

/// The interface the kernel would send from, asked by connecting a UDP socket - which sends nothing
/// - and reading back the source it chose. Empty when it cannot be worked out.
std::string outgoingInterface(const std::string& serverHost, uint16_t serverPort)
{
#ifndef __linux__
    // The name of the interface is looked up by ReceiveSteering, which only knows Linux, so on any
    // other system there is nothing to be found - and the calls below are POSIX ones that Windows
    // does not have under those names.
    (void) serverHost;
    (void) serverPort;
    return {};
#else
    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverHost.c_str(), &target.sin_addr) != 1)
    {
        return {};
    }
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0)
    {
        return {};
    }
    std::string interfaceName;
    if (connect(probe, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0)
    {
        sockaddr_in chosen {};
        socklen_t   length = sizeof(chosen);
        if (getsockname(probe, reinterpret_cast<sockaddr*>(&chosen), &length) == 0)
        {
            std::array<char, INET_ADDRSTRLEN> text {};
            if (inet_ntop(AF_INET, &chosen.sin_addr, text.data(), text.size()) != nullptr)
            {
                const auto names = xmq::ReceiveSteering::interfacesForAddresses({text.data()});
                if (!names.empty())
                {
                    interfaceName = names.front();
                }
            }
        }
    }
    close(probe);
    return interfaceName;
#endif
}

} // namespace

void Scenario::executeScenario() const
{
    filesystem::path scenarioFile(commandLine().getOptionValue("scenario").c_str());
    if (scenarioFile.empty())
    {
        throw Exception("Scenario file is not provided. Use the --scenario option.");
    }

    scenarioFile = resolveScenarioFile(scenarioFile, m_executableDirectory);

    ScenarioEngine engine;
    engine.logEngine(logEngine());
    engine.load(scenarioFile);

    overrideScenarioParameters(engine.scenario());
    engine.showProgress(commandLine().hasOption("progress"));
    // Null unless --encrypted, --cafile or --cert/--key were given; see RunDefinition::load().
    engine.sslKeys(runDefinition().m_sslKeys);

    // Whatever "-D connect ..." and "-D publish ..." asked for. Scenarios carry no properties of
    // their own, so this is the only way to measure what MQTT5 properties cost in a load test -
    // and the only way to compare brokers on anything but the plainest MQTT3 traffic.
    engine.commandProperties(runDefinition().m_commandConnectProperties,
                             runDefinition().m_commandPublishProperties);

    if (const auto connectIntervals = commandLine().getOptionValue("connect-intervals").toInt();
        connectIntervals > 0)
    {
        engine.connectIntervals(static_cast<size_t>(connectIntervals));
    }

    // --result-interval NNNs | NNNm. The command line has already checked the shape, so the last
    // character is the unit and everything before it is the count.
    if (const auto resultInterval = commandLine().getOptionValue("result-interval");
        !resultInterval.empty())
    {
        const auto count = String(resultInterval.substr(0, resultInterval.length() - 1)).toInt();
        const auto unit = resultInterval.back() == 'm' ? 60 : 1;
        engine.reportInterval(chrono::seconds(count * unit));
    }

    // Two ways the traffic can leave through something that is not the network being measured: the
    // addresses bound to may sit on a tunnel, or the route to the broker may take one anyway.
    const auto& server = engine.scenario().m_server;
    if (const auto outgoing = outgoingInterface(std::string(server.m_hostname.asString()), static_cast<uint16_t>(server.m_port.asInteger()));
        !outgoing.empty() && looksLikeTunnel(outgoing))
    {
        CERR(format("WARNING: traffic to {} leaves through {}, which is a tunnel. What gets measured "
                    "is the tunnel.", std::string(server.m_hostname.asString()), outgoing));
    }



    if (const auto bindInterfaces = commandLine().getOptionValue("bind-to-interfaces");
        !bindInterfaces.empty())
    {
        const auto addresses = Scenario::resolveBindInterfaces(bindInterfaces);
        if (commandLine().hasOption("verbose"))
        {
            COUT(format("Binding clients to {} local interface(s): {}", addresses.size(), addresses.join(", ").c_str()));
        }
        else
        {
            COUT(format("Binding clients to {} local interface(s).", addresses.size()));
        }
        for (const auto& name: xmq::ReceiveSteering::interfacesForAddresses(
                 std::vector<std::string>(addresses.begin(), addresses.end())))
        {
            if (looksLikeTunnel(name))
            {
                CERR(format("WARNING: bound addresses live on {}, which is a tunnel.", name));
            }
        }
        engine.bindAddresses(addresses);
    }

    vector<RoundTripLatency> clientPublishLatencies;

    // Guarantees engine.disconnectClients() runs before clientPublishLatencies is destroyed, even
    // if connectClients()/publish() throws (e.g. MqttClient::sendMessage() throwing "Not connected"
    // for a publisher that dropped mid-run, which is plausible under sustained high connection
    // counts). Without this, subscriber onMessage callbacks captured by reference into this vector
    // (see ScenarioEngine::subscribeClients()) could still be in flight when the exception unwinds
    // past the explicit disconnectClients() call below, touching freed memory. Declared after
    // clientPublishLatencies so its destructor - which is what actually calls disconnectClients() -
    // runs first (C++ destroys locals in reverse declaration order).
    struct DisconnectGuard
    {
        ScenarioEngine& engine;

        ~DisconnectGuard()
        {
            engine.disconnectClients();
        }
    } disconnectGuard {engine};

    engine.connectClients(clientPublishLatencies, engine.name());
    if (ScenarioEngine::typeFromString(engine.type()) != ScenarioEngine::Type::Connections)
    {
        engine.publish(clientPublishLatencies, engine.name());
    }
}

void Scenario::overrideScenarioParameters(CTestScenario& scenario) const
{
    const auto& arguments = dynamic_cast<const ScenarioCommandLine&>(commandLine());

    if (arguments.optionSpecified("host"))
    {
        scenario.m_server.m_hostname = arguments.getOptionValue("host");
    }
    if (arguments.optionSpecified("port"))
    {
        scenario.m_server.m_port = arguments.getOptionValue("port").toInt();
    }
    if (arguments.optionSpecified("username"))
    {
        scenario.m_server.m_username = arguments.getOptionValue("username");
    }
    if (arguments.optionSpecified("password"))
    {
        scenario.m_server.m_password = arguments.getOptionValue("password");
    }

    // The subscriber group's own broker, for bridge testing: publishers go to the scenario's
    // server, subscribers to this one. Overriding it here keeps the host names out of the
    // shipped scenario files, which would otherwise have to name two machines.
    if (arguments.optionSpecified("subscriber-host") || arguments.optionSpecified("subscriber-port"))
    {
        auto& subscriberServer = scenario.m_subscribers.m_server;
        if (subscriberServer.m_hostname.asString().empty())
        {
            // Start from the scenario's server, so only what is overridden differs.
            subscriberServer = scenario.m_server;
        }
        if (arguments.optionSpecified("subscriber-host"))
        {
            subscriberServer.m_hostname = arguments.getOptionValue("subscriber-host");
        }
        if (arguments.optionSpecified("subscriber-port"))
        {
            subscriberServer.m_port = arguments.getOptionValue("subscriber-port").toInt();
        }
    }

    if (arguments.optionSpecified("payload-size"))
    {
        scenario.m_parameters.m_payload_size = arguments.getOptionValue("payload-size").toInt();
    }
    if (arguments.optionSpecified("publish-rate"))
    {
        scenario.m_parameters.m_publish_rate = arguments.getOptionValue("publish-rate").toInt();
    }
    if (arguments.optionSpecified("publish-count"))
    {
        scenario.m_parameters.m_message_count = arguments.getOptionValue("publish-count").toInt();
    }
    if (arguments.optionSpecified("duration"))
    {
        scenario.m_parameters.m_duration_sec = arguments.getOptionValue("duration").toInt();
    }
    if (arguments.optionSpecified("connection-rate"))
    {
        scenario.m_parameters.m_connection_rate = arguments.getOptionValue("connection-rate").toInt();
    }
    if (arguments.optionSpecified("keep-alive"))
    {
        scenario.m_parameters.m_keep_alive_sec = arguments.getOptionValue("keep-alive").toInt();
    }
    if (arguments.optionSpecified("max-inflight"))
    {
        scenario.m_parameters.m_max_inflight_messages = arguments.getOptionValue("max-inflight").toInt();
    }
    if (arguments.optionSpecified("qos"))
    {
        const auto qos = arguments.getOptionValue("qos").toInt();
        scenario.m_publishers.m_qos = qos;
        scenario.m_subscribers.m_qos = qos;
    }
    if (arguments.optionSpecified("protocol-version"))
    {
        const auto protocolVersion = arguments.getOptionValue("protocol-version").toInt();
        scenario.m_publishers.m_protocol_version = protocolVersion;
        scenario.m_subscribers.m_protocol_version = protocolVersion;
    }
    if (arguments.optionSpecified("id-prefix"))
    {
        // Prepended, not replaced: publishers and subscribers keep their own scenario-file
        // prefixes (e.g. "test-publisher-" vs "test-subscriber-") so they stay distinguishable
        // on the same host; the CLI value just adds a per-host tag in front of both, so running
        // the same scenario from several hosts against one broker doesn't collide client IDs.
        const auto idPrefix = arguments.getOptionValue("id-prefix");
        scenario.m_publishers.m_id_prefix = idPrefix + scenario.m_publishers.m_id_prefix.asString();
        scenario.m_subscribers.m_id_prefix = idPrefix + scenario.m_subscribers.m_id_prefix.asString();
    }
}
