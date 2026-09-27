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

#include "SocketDataReader.h"

using namespace std;
using namespace sptk;

namespace xmq {

void readWithTimeout(SocketDataReader& socket, uint8_t* buffer, const size_t size, const chrono::milliseconds& timeout)
{
    auto* packetData = buffer;
    auto  bytesToRead = size;
    while (bytesToRead > 0)
    {
        if (const auto received = socket.read(packetData, bytesToRead);
            received == 0)
        {
            if (!socket.readyToRead(timeout))
            {
                throw Exception("Read timeout");
            }
        }
        else
        {
            packetData += received;
            bytesToRead -= received;
        }
    }
}

} // namespace xmq
