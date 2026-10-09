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

#include "ClusterTestRunner.h"

using namespace std;
using namespace sptk;
using namespace xmq;

int main(const int argc, const char* argv[])
{
    const vector<string> args(argv, argv + argc);
    try
    {
        ClusterTestRunner runner(args);
        return runner.run();
    }
    catch (const Exception& e)
    {
        CERR(e.message());
    }
    return 1;
}
