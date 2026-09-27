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
#include "ConnectionsCommandLine.h"
#include <queue>

using namespace std;
using namespace sptk;

namespace xmq {

ConnectionsCommandLine::ConnectionsCommandLine(const std::vector<std::string>& args)
    : UtilityCommandLine("xmq_con " + sptk::String(XMQ_VERSION_NUMBER), "MQTT version 5/3.1.1/3.1 client for Connections tests.",
                         "xmq_con [options]")
{
    const Visibility alwaysVisible("");
    defineParameter("connect-rate", "R", "connect rate", R"(^\d+$)", alwaysVisible, "", "Connect rate per second.");
    defineOption("quiet", "", alwaysVisible, "Don't print to stdout.");
    defineOption("debug", "d", alwaysVisible, "Print debug messages.");
    defineOption("help", "?", alwaysVisible, "Print this help.");

    addNote("Connect properties",
            "authentication-data          (binary data - note treated as a string)\n"
            "authentication-method        (UTF-8 string pair)\n"
            "maximum-packet-size          (32-bit unsigned integer)\n"
            "receive-maximum              (16-bit unsigned integer)\n"
            "request-problem-information  (8-bit unsigned integer)\n"
            "request-response-information (8-bit unsigned integer)\n"
            "session-expiry-interval      (32-bit unsigned integer, note use -x instead)\n"
            "topic-alias-maximum          (16-bit unsigned integer)\n"
            "user-property                (UTF-8 string pair)");

    initCommandLine(args);
}

} // namespace xmq
