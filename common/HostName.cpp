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

#include "HostName.h"

#include <array>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;

namespace xmq {

String thisHostName()
{
    constexpr size_t               maxHostNameLength = 256;
    array<char, maxHostNameLength> hostName {};

    if (gethostname(hostName.data(), static_cast<int>(hostName.size() - 1)) != 0 || hostName[0] == '\0')
    {
        return "localhost";
    }
    return {hostName.data()};
}

} // namespace xmq
