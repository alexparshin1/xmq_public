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

#include "base/xmq-config.h"
#include "ClusterTestCommandLine.h"

#include <map>

using namespace std;
using namespace sptk;

namespace xmq {

ClusterTestCommandLine::ClusterTestCommandLine(const vector<string>& args)
    : UtilityCommandLine("xmq_scn_cluster " + String(XMQ_VERSION_NUMBER),
                         "MQTT cluster test orchestrator: runs a load scenario against a cluster while the "
                         "nodes of it are taken away and brought back, then checks what the cluster did "
                         "about it",
                         "xmq_scn_cluster [options]")
{
    const Visibility alwaysVisible("");
    defineParameter("scenario", "s", "file", "", alwaysVisible, "",
                    "Cluster test JSON file: the nodes, what happens to them and when, the load scenario "
                    "to run, and the checks. A relative path is looked up in the current directory "
                    "first. Examples live in LoadCluster.local.");
    defineParameter("load-scenario", "", "file", "", alwaysVisible, "",
                    "Run this scenario instead of the one the test file names, with the same cluster and "
                    "the same timeline. For trying a different load against a stand that is already "
                    "understood.");
    defineParameter("bind-to-interfaces", "I", "IP list or mask", R"(^[\d\.,/]+$)", alwaysVisible, "",
                    "Bind the load's clients to local interface IP addresses, using round-robin. "
                    "Either a comma-separated IP address list, e.g. 10.1.1.24,10.1.1.100, "
                    "or a mask selecting matching local interfaces, e.g. 10.1.1.1/8. "
                    "Use it to create more than 64K clients.");
    defineParameter("connect-intervals", "", "number", R"(^\d+$)", alwaysVisible, "10",
                    "How many rows the connect phase reports, default 10. The publish phase takes a "
                    "width instead (--result-interval); the connect phase cannot, because it ends when "
                    "the last client is connected and its length is not known in advance.");
    defineOption("dry-run", "", alwaysVisible,
                 "Read the test file, print the plan and the checks, and exit without running anything. "
                 "A test takes minutes, and a mistake in it is cheaper to hear about here.");
    defineOption("help", "", alwaysVisible, "Print this help.");

    addNote("Note",
            "--host, --port, --username and --password override the server the test file names, which is "
            "how one test is pointed at another stand. The load's own clients take their settings from "
            "the scenario file, as they do under xmq_scn.");

    initCommandLine(args);

    // sptk::CommandLine keeps parameter defaults in the same map as parsed values, so hasOption()
    // cannot tell an explicit argument from a default. Record which options were on the command line:
    // only those may override the test file.
    static const map<string, string, less<>> shortToLongName = {
        {"h", "host"}, {"p", "port"}, {"u", "username"}, {"P", "password"}, {"s", "scenario"},
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
