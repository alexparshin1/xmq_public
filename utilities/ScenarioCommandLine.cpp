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

#include "base/xmq-config.h"
#include "ScenarioCommandLine.h"
#include <map>
#include <queue>

using namespace std;
using namespace sptk;

namespace xmq {

ScenarioCommandLine::ScenarioCommandLine(const std::vector<std::string>& args)
    : UtilityCommandLine("xmq_scn " + sptk::String(XMQ_VERSION_NUMBER), "MQTT version 5/3.1.1/3.1 client for Scenario tests",
                         "xmq_scn [options]")
{
    const Visibility alwaysVisible("");
    defineParameter("scenario", "s", "file", "", alwaysVisible, "",
                    "Scenario JSON file. An absolute path is used as is. A relative path is looked up "
                    "in the current directory first, then in the installed scenario directory "
                    "(share/xmq next to this utility's executable), e.g. '1M-Connections-5K-rate.json'.");
    defineParameter("subscriber-host", "", "host", "", alwaysVisible, "",
                    "Broker the subscribers connect to, when it differs from the publishers'. "
                    "Used to test bridging: messages published on the scenario's server must "
                    "reach subscribers on this one. Defaults to the scenario's server.");
    defineParameter("subscriber-port", "", "number", R"(^\d+$)", alwaysVisible, "",
                    "Port of the subscribers' broker. Defaults to the scenario's server port.");
    defineParameter("payload-size", "m", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Message size.");
    defineParameter("publish-rate", "r", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Messages send rate per publisher, messages per second. Total scenario throughput is "
                    "this rate multiplied by the publisher count. 0 (the default) means unpaced: publish as fast as possible.");
    defineParameter("publish-count", "C", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Number of messages to publish.");
    defineParameter("duration", "d", "seconds", R"(^\d+$)", alwaysVisible, "0",
                    "Test duration, seconds. When set, publishing continues until the duration expires.");
    defineParameter("connection-rate", "R", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Client connection rate, connections per second.");
    defineParameter("qos", "q", "QOS", R"(^(0|1|2)$)", alwaysVisible, "0",
                    "Message QOS: {0,1,2}.");
    defineParameter("bind-to-interfaces", "I", "IP list or mask", R"(^[\d\.,/]+$)", alwaysVisible, "",
                    "Bind clients to local interface IP addresses, using round-robin. "
                    "Either a comma-separated IP address list, e.g. 10.1.1.24,10.1.1.100, "
                    "or a mask selecting matching local interfaces, e.g. 10.1.1.1/8. "
                    "Use it to create more than 64K clients.");
    defineParameter("max-inflight", "", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Maximum un-acked QoS1/2 messages in flight per client before further sends queue "
                    "locally instead of going out immediately. 0 (the default) uses the client "
                    "library's own default (32768, effectively unbounded for typical publish rates). "
                    "Lower this to have each publisher throttle against its own ack round-trip time "
                    "instead of sending at publish-rate regardless of whether the broker keeps up.");
    defineParameter("connect-intervals", "", "number", R"(^\d+$)", alwaysVisible, "10",
                    "How many rows the connect phase reports, default 10. The publish phase takes a "
                    "width instead (--result-interval); the connect phase cannot, because it ends "
                    "when the last client is connected and its length is not known in advance. Raise "
                    "it to see where within the connect phase something happens.");
    defineParameter("id-prefix", "", "prefix", "", alwaysVisible, "",
                    "Prepend to both publishers' and subscribers' id_prefix from the scenario file "
                    "(unlike other overrides, this is prepended rather than replacing the scenario "
                    "value, so publisher and subscriber client IDs stay distinguishable). Use a "
                    "distinct value per host when running the same scenario from multiple hosts "
                    "against one broker, so client IDs don't collide across hosts.");
    defineOption("help", "", alwaysVisible, "Print this help.");
    defineOption("disable-clean-session", "c", alwaysVisible, "Disable clean session.");
    defineOption("list-scenarios", "", alwaysVisible,
                 "List available scenario files, grouped by sub-directory, and exit.");
    defineOption("progress", "", alwaysVisible,
                 "Show a live, in-place progress bar for the connect and publish phases. "
                 "Off by default; leave off when redirecting output to a file or log.");
#ifdef __linux__
    // Only where it means something: FreeBSD has affinity but not the interfaces this reads, and
    // macOS has no way to pin a thread to a core at all.
    defineOption("use-cpu-affinity", "", alwaysVisible,
                 "Confine this client to the CPUs worth running on: the fastest cores where a machine "
                 "has more than one kind, and one thread per core where a core carries two. By default "
                 "CPU scheduling is left entirely to the OS. Pinning can improve measurement steadiness "
                 "on machines where moving between unlike cores changes speed and loses cache.");
#endif
    defineOption("verbose", "", alwaysVisible,
                 "Print full detail (e.g. every bound local interface address) instead of a "
                 "concise summary.");

    addNote("Note",
            "--encrypted, or either of --cafile and --cert/--key, runs the whole scenario over TLS.\n"
            "In that case, use the SSL port number (defaults to 8883). Otherwise, use the TCP port number (defaults to 1883).");

    initCommandLine(args);

    // sptk::CommandLine stores parameter defaults in the same map as parsed values,
    // so hasOption() cannot tell an explicit argument from a default. Record which
    // options were actually present on the command line - only those may override
    // the scenario file.
    static const map<string, string, less<>> shortToLongName = {
        {"h", "host"},
        {"p", "port"},
        {"u", "username"},
        {"P", "password"},
        {"s", "scenario"},
        {"m", "payload-size"},
        {"r", "publish-rate"},
        {"C", "publish-count"},
        {"d", "duration"},
        {"R", "connection-rate"},
        {"k", "keep-alive"},
        {"q", "qos"},
        {"V", "protocol-version"}};

    for (const auto& arg: args)
    {
        if (arg.starts_with("--"))
        {
            m_specifiedOptions.emplace(arg.substr(2));
        }
        else if (arg.starts_with('-') && arg.length() > 1)
        {
            if (const auto it = shortToLongName.find(arg.substr(1)); it != shortToLongName.end())
            {
                m_specifiedOptions.emplace(it->second);
            }
        }
    }
}

} // namespace xmq
