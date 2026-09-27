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

#include "UtilityCommandLine.h"

#include <queue>

using namespace std;
using namespace sptk;

namespace xmq {

RegularExpression        UtilityCommandLine::m_parseURI(R"(^(?<protocol>mqtt|mqtts)://((?<user>[^@:]+)(?<pass>:[^@]+)?@)?(?<host>[^:/]+)(?<port>:\d+)?(?<topic>/.*)?$)");
std::vector<std::string> UtilityCommandLine::m_processedArguments;

std::vector<const char*> UtilityCommandLine::PreprocessCommandLine(const std::vector<std::string>& args)
{
    queue<string> argsQueue;
    for (const auto& arg: args)
    {
        argsQueue.push(arg);
    }

    if (argsQueue.size() == 1)
    {
        argsQueue.emplace("--help");
    }

    std::vector<const char*> processedArgumentPointers;

    m_processedArguments.clear();

    Strings properties;
    while (!argsQueue.empty())
    {
        auto arg = argsQueue.front();
        argsQueue.pop();

        if (arg == "-D")
        {
            constexpr size_t expectedArguments = 3;
            if (argsQueue.size() < expectedArguments)
            {
                throw Exception("Invalid -D argument: Expected property command, key, and value.");
            }
            // Taken off the queue, not merely read from its head. Without the pop the same word
            // was collected three times - "-D connect receive-maximum 10" became the property
            // "connect:connect=connect" - and the three arguments stayed in the queue to be
            // rejected as unexpected. The option has never worked.
            Strings arguments;
            for (size_t i = 0; i < expectedArguments; ++i)
            {
                arguments.push_back(argsQueue.front());
                argsQueue.pop();
            }
            stringstream   propertyData;
            constexpr auto propName = 1;
            constexpr auto propValue = 2;
            propertyData << arguments[0] << ":" << arguments[propName] << "=" << arguments[propValue];
            properties.emplace_back(propertyData.str());
        }
        else
        {
            m_processedArguments.push_back(arg);
        }
    }

    if (!properties.empty())
    {
        m_processedArguments.emplace_back("-D");
        m_processedArguments.emplace_back(properties.join(";"));
    }

    (void) ranges::all_of(m_processedArguments,
                          [&processedArgumentPointers](const string_view str)
                          {
                              processedArgumentPointers.push_back(str.data());
                              return true;
                          });


    return processedArgumentPointers;
}

const RegularExpression& UtilityCommandLine::uriParser()
{
    return m_parseURI;
}

UtilityCommandLine::UtilityCommandLine(const String& programVersion, const String& description, const String& commandLinePrototype)
    : CommandLine(programVersion, description, commandLinePrototype)
{
    const Visibility alwaysVisible("");
    defineParameter("bind-address", "A", "ip addresses",
                    R"([\d\.\,]+)", alwaysVisible, "",
                    "Local interface IP addresses to bind to, separated with comma.");
    defineParameter("cafile", "", "CA file", "", alwaysVisible, "",
                    "Path to CA certificates file.");
    defineParameter("key", "", "key file", "", alwaysVisible, "",
                    "Path to client private key file.");
    defineParameter("cert", "", "certificate file", "", alwaysVisible, "",
                    "Path to client certificate file.");
    defineOption("encrypted", "", alwaysVisible,
                 "Connect over TLS. Needed on its own only when no client certificate is used - a "
                 "listener that does not ask for one still encrypts, and --cafile or --cert/--key "
                 "imply this option. Remember the encrypted listener is on a different port.");
    defineParameter("client-id", "i", "client id", "", alwaysVisible, "test-client",
                    "Client id.");
    defineParameter("host", "h", "hostname", "^\\S+$", alwaysVisible, "localhost",
                    "Server hostname.");
    defineParameter("keep-alive", "k", "seconds", R"(^\d+$)", alwaysVisible, "0",
                    "Keep alive seconds, 0 or omit to disable.");
    defineParameter("result-interval", "", "NNNs or NNNm", R"(^\d+[sm]$)", alwaysVisible, "",
                    "How much time each reported result row covers, e.g. 30s or 1m. "
                    "Omit to divide the run into ten intervals, whose length then depends on how "
                    "long the run is.");
    defineParameter("password", "P", "password", "", alwaysVisible, "",
                    "Password.");
    defineParameter("port", "p", "port #", "^\\d{2,5}$", alwaysVisible, "1883",
                    "Server port number.");
    // The pattern has to accept what this class assembles, not what the user types: several "-D"
    // options become one value, "connect:receive-maximum=10;publish:user-property=alpha=beta". So
    // ';' separates definitions and a value may hold '=' - a user property is a pair, and its value
    // is "name=value". The old pattern allowed neither, which rejected every use of the option with
    // more than one property and every use of a user property at all.
    defineParameter("property", "D", "message, key, and value",
                    R"(^\w+:[\w\-]+=[^;]+(;\w+:[\w\-]+=[^;]+)*$)", alwaysVisible, "",
                    "Command properties. See Command Properties section below. Possible example is '-D connect receive-maximum 10'. "
                    "This parameter can be specified multiple times.");
    defineParameter("protocol-version", "V", "protocol version", R"(^[3-5]$)", alwaysVisible, "3",
                    "Protocol version: {3,4,5}.");
    defineParameter("sessions", "n", "number", "^\\d+$", alwaysVisible, "1",
                    "Number of sessions to connect.");
    defineParameter("url", "L", "URL", uriParser().pattern(), alwaysVisible, "",
                    "Session URL. Possible example is mqtt://user:password@127.0.0.1:1884/topic1.");
    defineParameter("username", "u", "user name", "[\\S]+$", alwaysVisible, "",
                    "User name.");
    defineOption("show-counters", "", alwaysVisible, "Show progress counters.");
    defineOption("show-counters-csv", "", alwaysVisible, "Show progress counters in CSV format.");
    // Defined on the shared base so every utility (xmq_scn, xmq_pub, xmq_sub, xmq_con)
    // answers --version identically; handled in Utility's constructor.
    defineOption("version", "", alwaysVisible, "Print the version and exit.");
}

void UtilityCommandLine::initCommandLine(const vector<std::string>& args)
{
    auto allArguments = PreprocessCommandLine(args);
    try
    {
        init(allArguments.size(), allArguments.data());
        if (!arguments().empty())
        {
            throw Exception("Unexpected arguments: " + arguments().join(", "));
        }
    }
    catch (const Exception& e)
    {
        m_error = e.message();
        CERR(e.message() << ".\n");
    }
}

} // namespace xmq
