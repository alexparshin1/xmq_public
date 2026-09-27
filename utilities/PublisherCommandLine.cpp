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
#include "PublisherCommandLine.h"
#include <queue>

using namespace std;
using namespace sptk;

namespace xmq {

PublisherCommandLine::PublisherCommandLine(const std::vector<std::string>& args)
    : UtilityCommandLine("xmq_pub " + sptk::String(XMQ_VERSION_NUMBER), "MQTT version 5/3.1.1/3.1 client for Publish tests",
                         "xmq_pub [options]")
{
    const Visibility alwaysVisible("");
    defineParameter("message", "m", "text", "", alwaysVisible, "",
                    "Message text.");
    defineParameter("qos", "q", "QOS", R"(^(0|1|2)$)", alwaysVisible, "0",
                    "Message QOS: {0,1,2}.");
    defineParameter("repeat", "r", "number", R"(^\d+$)", alwaysVisible, "1",
                    "Message repeat count.");
    defineParameter("session-expiry-interval", "x", "seconds", R"(^\d+$)", alwaysVisible, "",
                    "Session expiry interval, the default is no expire.");
    defineParameter("message-file", "f", "file name", "", alwaysVisible, "",
                    "Send file content as message.");
    defineParameter("max-inflight", "M", "max inflight", R"(^\d+$)", alwaysVisible, "128",
                    "The maximum inflight messages for QoS 1/2.");
    defineParameter("message-rate", "g", "number", R"(^\d+$)", alwaysVisible, "0",
                    "Messages send rate, messages per second.");
    defineParameter("topic", "t", "topic", "^\\w[\\w/\\-%,]*$", alwaysVisible, "",
                    "Message topic. Multiple topics can be separated by semicolon. The topic may include %ClientIndex% that is substituted by the client index (number).");
    // -r is already the repeat count, so retain takes -R.
    defineOption("retain", "R", alwaysVisible,
                 "Publish as a retained message: the broker keeps the last one per topic and "
                 "gives it to each new subscriber.");
    defineOption("stdin", "s", alwaysVisible, "Read messages from stdin, one message for per line.");
    defineOption("quiet", "", alwaysVisible, "Don't print to stdout.");
    defineOption("debug", "d", alwaysVisible, "Print debug messages.");
    defineOption("help", "", alwaysVisible, "Print this help.");
    defineOption("disable-clean-session", "c", alwaysVisible, "Disable clean session.");
    defineOption("send-timestamp", "l", alwaysVisible, "Inject a send timestamp at the beginning of each message. Used by xmq_sub for latency measurements.");

    addNote("Note",
            "If both, cafile and cert options, are provided - the SSL connection if attempted.\n"
            "In that case, use the SSL port number (defaults to 8883). Otherwise, use the TCP port number (defaults to 1883).");

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

    addNote("Publish properties", "content-type                 (UTF-8 string)\n"
                                  "correlation-data             (binary data - note treated as a string in mosquitto_pub)\n"
                                  "message-expiry-interval      (32-bit unsigned integer)\n"
                                  "payload-format-indicator     (8-bit unsigned integer)\n"
                                  "response-topic               (UTF-8 string)\n"
                                  "topic-alias                  (16-bit unsigned integer)\n"
                                  "user-property                (UTF-8 string pair)");

    initCommandLine(args);
}

} // namespace xmq
