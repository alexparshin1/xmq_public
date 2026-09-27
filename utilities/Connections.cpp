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

#include "Connections.h"
#include "ConnectionsCommandLine.h"

using namespace std;
using namespace sptk;
using namespace xmq;

Connections::Connections(const vector<string>& args)
    : Utility(make_shared<ConnectionsCommandLine>(args))
{
}

int Connections::run()
{
    if (commandLine().hasOption("help"))
    {
        constexpr int defaultScreenWidth = 80;
        const auto*   colsEnv = getenv("COLS");
        const size_t  screenCols = colsEnv == nullptr ? defaultScreenWidth : string2int(colsEnv);
        commandLine().printHelp(screenCols);
        return 1;
    }

    Logger logger(*logEngine());

    createClients();

    connectClients();

    COUT("Press Enter to disconnect clients...");

    getchar();

    disconnectClients();

    return 0;
}

int main(const int argc, const char* argv[])
{
    const vector<string> args(argv, argv + argc);
    try
    {
        // Inside the try, not before it. The constructor reads the command line, and everything it
        // refuses - an unknown property, a number given a word, properties asked for on a protocol
        // version that has none - used to leave the process on an unhandled exception instead of a
        // message.
        Connections          publisher(args);
        return publisher.run();
    }
    catch (const Exception& e)
    {
        CERR(e.message());
    }
    return 1;
}
