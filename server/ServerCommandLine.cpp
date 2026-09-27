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

#include "ServerCommandLine.h"

#include "base/xmq-config.h"
#include "common/DirectoryNames.h"

using namespace std;
using namespace sptk;
using namespace xmq;

ServerCommandLine::ServerCommandLine()
    : CommandLine("XMQ Server " + String(XMQ_VERSION_NUMBER),
                  "Message Queue Server. Copyright (C) 2022-2025 Alexey Parshin",
                  "xmq_server [options]")
{
    const auto       defaultConfigPath = DirectoryNames::confDirectory() / "xmq_server.conf";
    const Visibility alwaysVisible("");
    defineParameter("configuration-file", "c", "configuration file", "", alwaysVisible,
                    defaultConfigPath.string(), "Configuration file path.");
    defineParameter("users-file", "U", "users file", "", alwaysVisible, "",
                    "Users file path. Defaults to 'xmq_users.conf' beside the configuration file, "
                    "so two servers with configurations in separate directories keep separate "
                    "accounts without being told to.");
    defineParameter("log-file", "l", "log file", "", alwaysVisible,
                    "xmq_server.log", "Log file path.");
    defineOption("reset", "r", alwaysVisible, "Reset persistent storage. Overrides configuration.");
    defineOption("reset-configuration", "", alwaysVisible,
                 "Replace the configuration file with a starting one and carry on: MQTT on 1883 and 8883, "
                 "the configuration interface on 18883, persistence off. The way back when a configuration "
                 "will not start and the interface that would fix it is what is not coming up. The old file "
                 "is kept beside it with an '.old' suffix. Accounts are not touched, and one for 'admin', "
                 "without a password, is created if there are none at all.");
    defineParameter("set-password", "", "account", "", alwaysVisible, "",
                    "Set the password of an account and exit. The password is read from standard "
                    "input, not from the command line, so it stays out of the process list and the "
                    "shell history. This is the only way to give the administrator a password "
                    "without the configuration interface - which matters wherever that interface "
                    "cannot be reached, a container being the clearest case: until the "
                    "administrator has a password the interface answers on the loopback address "
                    "only, and a container's loopback is not the host's.");
    defineOption("debug", "d", alwaysVisible, "Debug logging. Overrides configuration.");
    defineOption("help", "h", alwaysVisible, "Show help");
    defineOption("version", "v", alwaysVisible, "Print the version and exit.");
#ifdef _WIN32
    defineOption("install-service", "i", alwaysVisible, "Install XMQ Windows service.");
    defineOption("uninstall-service", "u", alwaysVisible, "Uninstall XMQ Windows service.");
    // Windows alone still has two ways to run, so it alone still needs to be told which. Everywhere
    // else the broker stays in the foreground and there is nothing to choose, so the option is gone
    // rather than accepted and ignored.
    defineOption("console", "s", alwaysVisible, "Start as a console application, not as a service.");
#endif
#ifdef __linux__
    // Only where it means something - see CpuAffinity::useBestCores().
    defineOption("use-cpu-affinity", "", alwaysVisible,
                 "Restrict the server to one hardware thread per physical CPU core. By default CPU "
                 "scheduling is left entirely to the OS. This option leaves hyperthread siblings unused, "
                 "which can help on machines where they contend for execution units instead of adding "
                 "throughput.");
#endif
}
