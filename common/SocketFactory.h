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

#pragma once

#include <sptk5/net/SSLKeys.h>
#include <sptk5/net/SSLSocket.h>
#include <sptk5/net/TCPSocket.h>

namespace xmq {

class SocketFactory
{
public:
    static std::shared_ptr<sptk::TCPSocket> createSocket(const std::shared_ptr<sptk::SSLKeys>& sslKeys)
    {
        if (sslKeys)
        {
            const auto sslSocket = std::make_shared<sptk::SSLSocket>("HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4", true);
            sslSocket->loadKeys(*sslKeys);
            return sslSocket;
        }

        return std::make_shared<sptk::TCPSocket>();
    }
};

} // namespace xmq
