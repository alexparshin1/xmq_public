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

#include "OSService.h"
#include "ServerController.h"
#include "base/DescriptorLimit.h"
#include "base/ThreadCount.h"
#include "base/CpuAffinity.h"
#include "base/xmq-config.h"
#include "common/DirectoryNames.h"

#include <iostream>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

#include "server/Server.h"
#include "server/ServerCommandLine.h"
#include "server/Settings/Settings.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

void signalHandler(const int theSignal)
{
    // Async-signal-safe: restore the default handler (so a second Ctrl+C force-terminates if a
    // graceful shutdown ever stalls) and request termination. The actual shutdown — which takes
    // locks and joins threads — runs on the service thread, not here.
    (void) signal(theSignal, SIG_DFL);
    OSService::requestTermination(theSignal);
}

#ifndef _WIN32
void reloadSignalHandler(int /*theSignal*/)
{
    // Async-signal-safe: only sets a flag. The actual reload - re-reading extension configuration
    // and rotating the log - runs on the service thread, not here. The handler stays installed
    // (unlike signalHandler above), so a second SIGHUP does another reload rather than falling
    // through to SIG_DFL's default action, which for SIGHUP is to terminate the process - the one
    // thing this signal must not do.
    OSService::requestReload();
}
#endif

/**
 * @brief The file the log engine will actually write to.
 *
 * The configured name is not always that file: a bare name is resolved against the logs
 * directory. Worked out here only to report it, because the engine itself belongs to the
 * controller - one process wrote its log through three engines over the same file, and each
 * one opening it discarded what the previous had written.
 *
 * @param settings          Server settings.
 * @return the log file path.
 */
filesystem::path resolveLogFilePath(const Settings& settings)
{
    // Asked of the controller rather than worked out again here. Written out twice, the two copies
    // agreed on being wrong: a bare name was replaced by xmq_server.log rather than resolved.
    return ServerController::logFilePath(settings);
}

/**
 * @brief Describes where the log is going, for the startup message.
 *
 * The engine always writes to stdout, so the file is the only part worth reporting - and it is
 * not always kept: the Docker image points it at /dev/null, where naming the file as the
 * destination promises a log nobody will find.
 * @param logFilePath       Log file the engine was given.
 * @return description of the log destinations.
 */
String logDestination(const filesystem::path& logFilePath)
{
    error_code error;
    const auto resolved = filesystem::canonical(logFilePath, error);
    if (!error && filesystem::is_character_file(resolved, error))
    {
        return "stdout only (" + logFilePath.string() + " is " + resolved.string() + ")";
    }
    return "stdout and " + logFilePath.string();
}

filesystem::path getServerConfigurationPath(const ServerCommandLine& serverCommandLine)
{
    // Default configuration path
    filesystem::path serverConfigurationPath = DirectoryNames::confDirectory() / "xmq_server.conf";

    if (serverCommandLine.hasOption("configuration-file"))
    {
        serverConfigurationPath = serverCommandLine.getOptionValue("configuration-file").c_str();
    }

    if (serverCommandLine.hasOption("reset-configuration"))
    {
        COUT("Configuration reset: " << Settings::resetConfiguration(serverConfigurationPath) << ".");
        return serverConfigurationPath;
    }

    // A missing configuration is created rather than refused. The interface is how a server is
    // configured, and a server that will not start serves no interface - so refusing here leaves
    // someone writing JSON by hand to reach the page that exists to save them from that.
    if (const auto created = Settings::createConfiguration(serverConfigurationPath);
        !created.empty())
    {
        COUT("Configuration file " << serverConfigurationPath.string()
                                   << " was not found, so a starting one was created (" << created << ").");
    }

    return serverConfigurationPath;
}

} // namespace

int main(int argc, const char* argv[])
{
    (void) signal(SIGTERM, signalHandler);
    (void) signal(SIGINT, signalHandler);
#ifndef _WIN32
    // Reload, not terminate - the conventional meaning for a daemon, and the one this used to get
    // wrong: SIGHUP shared signalHandler with SIGTERM/SIGINT and stopped the broker silently.
    signal(SIGHUP, reloadSignalHandler);
#endif
    // For some reason, on Windows only, argc becomes 0 and argv becomes null
    // on the first use of argc. Making a copy of both:
    int         argcCopy = argc;
    const char* argvCopy[128];
    memcpy(argvCopy, argv, sizeof(const char*) * argc);

    ServerCommandLine serverCommandLine;
    try
    {
        serverCommandLine.init(argcCopy, argvCopy);
    }
    catch (const Exception& exception)
    {
        // An unrecognised option throws here. Without this catch it propagates out of main()
        // and the process aborts through std::terminate, which reads as a crash rather than
        // as the usage error it is.
        CERR(exception.message());
        CERR("Run 'xmq_server --help' for the list of options.");
        return 1;
    }

    // Answer --version before anything that can fail (configuration, storage, ports), so it
    // stays usable for recording the broker version alongside benchmark results.
    if (serverCommandLine.hasOption("version"))
    {
        COUT(XMQ_VERSION_NUMBER);
        return 0;
    }

    if (serverCommandLine.hasOption("help"))
    {
        constexpr auto helpWidth = 120;
        serverCommandLine.printHelp(helpWidth);
#ifdef _WIN32
        COUT("Note:");
        COUT("On Windows, starting XMQ server without command line arguments attempts to start a service.");
        COUT("If you need to start XMQ server as a console application, add --console argument");
#endif
        return 1;
    }

    // Before the server is brought up, and before the configuration path is created or reset:
    // this changes an account and exits, and is expected to be run while a server is installed
    // but not running - or running, in which case it will pick the change up when it next reads
    // the accounts.
    if (serverCommandLine.hasOption("set-password"))
    {
        const String account = serverCommandLine.getOptionValue("set-password");
        String       password;
        // From standard input rather than the command line: an argument is visible in the process
        // list to every user on the machine, and lands in the shell history besides. Typed at a
        // terminal - `docker exec -it`, for one - it is asked for and not echoed, as passwd does.
#ifndef _WIN32
        termios   savedTerminal {};
        const bool fromTerminal = isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &savedTerminal) == 0;
        if (fromTerminal)
        {
            cerr << "Password for " << account << ": " << flush;
            termios silent = savedTerminal;
            silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &silent);
        }
#endif
        const bool gotPassword = static_cast<bool>(getline(cin, password));
#ifndef _WIN32
        if (fromTerminal)
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &savedTerminal);
            cerr << endl;
        }
#endif
        if (!gotPassword)
        {
            CERR("No password on standard input. Pipe it in, as in: "
                 "printf '%s' \"$PASSWORD\" | xmq_server --set-password admin");
            return 1;
        }
        try
        {
            const filesystem::path configurationPath = serverCommandLine.hasOption("configuration-file")
                                                           ? filesystem::path(serverCommandLine.getOptionValue(
                                                                 "configuration-file").c_str())
                                                           : DirectoryNames::confDirectory() / "xmq_server.conf";
            const filesystem::path usersPath = serverCommandLine.getOptionValue("users-file").c_str();
            COUT(Settings::setAccountPassword(configurationPath, usersPath, account, password) << ".");
        }
        catch (const Exception& exception)
        {
            CERR("Can't set the password: " << exception.message());
            return 1;
        }
        return 0;
    }

    filesystem::path serverConfigurationPath;

    auto settings = make_shared<Settings>();
    try
    {
        serverConfigurationPath = getServerConfigurationPath(serverCommandLine);
        const filesystem::path usersPath = serverCommandLine.getOptionValue("users-file").c_str();
        settings->loadConfiguration(serverConfigurationPath, usersPath);
    }
    catch (const Exception& exception)
    {
        CERR("Can't load configuration file " << serverConfigurationPath.string() << ": " << exception.message());
        return 1;
    }

    if (serverCommandLine.hasOption("debug"))
    {
        settings->setLogPriority(LogPriority::Debug);
    }

    // Before anything creates a thread: affinity is inherited, so narrowing it here covers every
    // thread the server goes on to start when explicitly requested.
    String cpuAffinityDescription;
    bool   cpuAffinityApplied = false;
#ifdef __linux__
    if (serverCommandLine.hasOption("use-cpu-affinity"))
    {
        cpuAffinityApplied = CpuAffinity::useBestCores(cpuAffinityDescription);
    }
    else
    {
        cpuAffinityDescription = "disabled by default; use --use-cpu-affinity to enable";
    }
#else
    cpuAffinityDescription = "not supported on this platform";
#endif

    const auto logFilePath = resolveLogFilePath(*settings);

    // Built before anything is logged, because the engine is its own: everything this process
    // writes goes through this one.
    ServerController xmqServerController(settings);
    const auto       logEngine = xmqServerController.getLogEngine();

    Logger logger(*logEngine, "(" + settings->m_cluster.m_this_node.m_node_name.asString() + ") ");

    try
    {
        logger.info("Configuration:        " + serverConfigurationPath.string());
        logger.info("Log to:               " + logDestination(logFilePath));
        logger.info("Log level:            " + LogEngine::priorityName(logEngine->minPriority()));

        if (!cpuAffinityDescription.empty())
        {
            logger.info(String("CPU affinity:         ") + (cpuAffinityApplied ? "" : "not applied - ") +
                        cpuAffinityDescription);
        }

        // The Redis URI stays in the configuration when persistence is switched off, so reporting
        // it on its own would name a database the server is not going to open.
        if (auto redisURI = settings->m_persistence.m_redis_uri.asString();
            settings->m_persistence.m_enabled.asBool() && !redisURI.empty())
        {
            RegularExpression matchPassword("(//.*)(:[^@]+@)");

            bool replaced = false;
            auto safeDatabaseURI = matchPassword.replaceAll(redisURI, "\\1@", replaced);

            logger.info("Persistence database: " + safeDatabaseURI);
        }
        else
        {
            logger.info("Persistence:          off");
        }

        // What the per-connection tables were sized for, and where the number came from. They are
        // reserved once, at the descriptor limit, because a hash table that grows while connections
        // arrive rebuilds itself in the middle of the run - which is visible as a step in the
        // latency of everything connecting at that moment. The reserve is not free (about 26 Mb of
        // resident memory at two million), and it is invisible: without this line it is a number in
        // ps that nothing explains.
        logger.info("Connection reserve:   " + to_string(possibleConnectionCount()) +
                    " sessions, from the file descriptor limit");

        // What "auto" came to, because a machine with a different core count answers differently
        // and nobody should have to work that out from the source to read a measurement.
        {
            const auto sendThreads = settings->m_server_limits.m_send_threads.asString();
            const auto receiveThreads = settings->m_server_limits.m_receive_threads.asString();
            const auto deliveryThreads = settings->m_server_limits.m_delivery_threads.asString();
            const auto automatic = isAutomaticThreadCount(sendThreads) || isAutomaticThreadCount(receiveThreads) ||
                                   isAutomaticThreadCount(deliveryThreads);
            logger.info("Threads:              " + to_string(resolveThreadCount(sendThreads)) + " send, " +
                        to_string(resolveReceiveThreadCount(receiveThreads)) + " receive" +
                        ", " + to_string(resolveDeliveryThreadCount(deliveryThreads)) + " delivery" +
                        (automatic ? " (auto, from " + to_string(CpuAffinity::physicalCoreCount()) +
                                         " physical core(s))"
                                   : ""));
        }

        logger.info("Initialization completed");

#ifdef _WIN32
        // Windows has two ways to run: under the service dispatcher, or in a terminal.
        const auto runAsService = !serverCommandLine.hasOption("console");
#else
        // Nowhere else. The broker used to put itself into the background unless --console was
        // given, and it forked to do it after its threads were already running: the child inherited
        // a pthread_once that a thread which no longer existed had left half-finished, and the
        // first call to gmtime_r - issuing the self-signed certificate - waited on it for ever.
        //
        // Nothing asked for that fork. rc.d hands the broker to daemon(8) and systemd runs it
        // Type=simple; both want a process that stays where it was started. With the fork gone
        // there is nothing left to choose, so --console is gone with it.
        const auto runAsService = false;
#endif

        OSService osService(*xmqServerController.getLogEngine(), "(xmq service) ", runAsService);
        OSService::setServerController(&xmqServerController);

#ifdef _WIN32
        if (serverCommandLine.hasOption("install-service"))
        {
            osService.installService();
            return 0;
        }

        if (serverCommandLine.hasOption("uninstall-service"))
        {
            osService.uninstallService();
            return 0;
        }
#endif

        osService.execute();
    }
    catch (const DatabaseException& exception)
    {
        CERR("Can't start XMQ. Redis: " << settings->m_persistence.m_redis_uri.asString() << ": " << exception.message());
        logger.error(String(exception.message()));
        return 1;
    }
    catch (const Exception& exception)
    {
        CERR("Can't start XMQ: " << exception.message());
        logger.error(String(exception.message()));
        return 1;
    }

    return 0;
}
