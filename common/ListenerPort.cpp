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

#include "ListenerPort.h"

#include <format>
#include <sptk5/Exception.h>
#include <sptk5/net/TCPSocket.h>

using namespace std;
using namespace sptk;

namespace xmq {

static bool isListenerPortAvailable(const std::uint16_t port)
{
    if (port == 0)
    {
        return false;
    }

    TCPSocket probe;
    try
    {
        probe.open(Host("localhost", port), Socket::OpenMode::CONNECT, false, chrono::milliseconds(250));
    }
    catch (const Exception&)
    {
        // Nothing accepted the connection, the port is free.
        return true;
    }
    probe.close();
    throw Exception(format("TCP port {} is already in use - another server is using it. ",
                           port));
}


void checkListenerPortAvailable(const std::string& listener, const std::uint16_t port)
{
    if (isListenerPortAvailable(port))
    {
        return;
    }

    throw Exception(format("{} cannot listen on port {}: the port is already in use by another process.",
                           listener, port));
}

} // namespace xmq
