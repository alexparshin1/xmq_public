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

#include "ClientSessionThread.h"

namespace xmq {

class ClientSessionReceiveThread final : public ClientSessionThread
{
public:
    /**
     * @brief Constructor.
     */
    ClientSessionReceiveThread(Server* server, sptk::LogEngine& logEngine)
        : ClientSessionThread(server, logEngine)
    {
    }
    /**
     * @brief Destructor.
     */
    ~ClientSessionReceiveThread() override = default;

protected:
    void processSession(SClientSession& clientSession) override;
};

using SClientSessionReceiveThread = std::shared_ptr<ClientSessionReceiveThread>;

} // namespace xmq
