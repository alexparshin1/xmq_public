/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "ListenerPort.h"

#include <cerrno>
#include <format>
#include <sptk5/Exception.h>
#include <sptk5/net/TCPSocket.h>

using namespace std;
using namespace sptk;

namespace xmq {

bool isListenerPortAvailable(const std::string& bindAddress, const std::uint16_t port)
{
    if (port == 0)
    {
        // Any port: nothing to be unavailable about.
        return true;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (bindAddress.empty())
    {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else
    {
        address.sin_addr.s_addr = inet_addr(bindAddress.c_str());
        // A name or an address this probe cannot parse is left to addListener(), which resolves
        // and reports it. Answering "in use" here would name the wrong problem.
        if (address.sin_addr.s_addr == INADDR_NONE && bindAddress != "255.255.255.255")
        {
            return true;
        }
    }

    // A bind probe, not a connect probe: it asks the kernel the same question addListener() is
    // about to ask, on the configured address, and it does not need a listener that already
    // accepts connections to answer. SO_REUSEPORT is deliberately absent - the broker's own
    // listeners set it, and it is exactly what lets a second broker bind the same port.
#ifdef _WIN32
    const auto socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        return true;
    }

    // Windows has no SO_REUSEPORT: there SO_REUSEADDR is what lets sockets share a port. The
    // probe must therefore ask for exclusive use, so a listener already bound with SO_REUSEADDR
    // still makes this fail.
    const BOOL exclusive = TRUE;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    const auto result = ::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const auto error = result == 0 ? 0 : ::WSAGetLastError();
    ::closesocket(socketHandle);
    return error != WSAEADDRINUSE;
#else
    const auto socketHandle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socketHandle < 0)
    {
        return true;
    }

    // SO_REUSEADDR, not SO_REUSEPORT: it lets the probe bind over a socket lingering in
    // TIME_WAIT, as the broker's own listeners are allowed to, without letting it share a port
    // with a live listener.
    int reuse = 1;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    const auto result = ::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const auto error = result == 0 ? 0 : errno;
    ::close(socketHandle);
    return error != EADDRINUSE;
#endif
}

void checkListenerPortAvailable(const std::string& listener, const std::string& bindAddress,
                                const std::uint16_t port)
{
    if (isListenerPortAvailable(bindAddress, port))
    {
        return;
    }

    throw Exception(format("{} cannot listen on {}:{}: the port is already in use by another process.",
                           listener, bindAddress.empty() ? "0.0.0.0" : bindAddress, port));
}

} // namespace xmq
