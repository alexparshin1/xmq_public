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
#include "SubscriberCommandLine.h"
#include <queue>

using namespace std;
using namespace sptk;

namespace xmq {

SubscriberCommandLine::SubscriberCommandLine(const std::vector<std::string>& args)
    : UtilityCommandLine("xmq_sub " + sptk::String(XMQ_VERSION_NUMBER), "MQTT version 5/3.1.1/3.1 client for Subscribe tests",
                         "xmq_sub [options]")
{
    const Visibility alwaysVisible("");

    defineParameter("disconnect-after", "W", "seconds", R"(^\d+$)", alwaysVisible, "0",
                    "Disconnect after waiting for messages for N seconds.");
    defineParameter("receive-count", "C", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Disconnect and exit after receiving the 'msg_count' messages.");
    defineParameter("session-expiry-interval", "x", "seconds", R"(^\d+$)", alwaysVisible, "",
                    "Session expiry interval, the default is no expire.");
    defineParameter("qos", "q", "QOS", R"(^(0|1|2)$)", alwaysVisible, "0",
                    "Message QOS: {0,1,2}.");
    // '+' and '#' are what a subscription is for; rejecting them here made every wildcard
    // subscribe fail with "Parameter topic has invalid value" before a packet was ever sent.
    defineParameter("topic", "t", "topic", R"(^[A-Za-z0-9_+#][A-Za-z0-9_/,%+#-]*$)", alwaysVisible, "",
                    "Message topic. Multiple topics can be separated by semicolon. The topic may include %ClientIndex% that is substituted by the client index (number).");

    defineOption("quiet", "", alwaysVisible, "Don't print messages to stdout.");
    defineOption("debug", "d", alwaysVisible, "Print debug messages.");
    defineOption("help", "", alwaysVisible, "Print this help.");
    defineOption("disable-clean-session", "c", alwaysVisible, "Disable clean session.");
    defineOption("print-messages", "v", alwaysVisible, "Print received messages.");

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
