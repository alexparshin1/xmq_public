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

#include "BatchSocketWriter.h"
#include "ClientSessionThread.h"
#include "ServerConnectionExt.h"

namespace xmq {

class ClientSessionSendThread final : public ClientSessionThread
{
public:
    /**
     * @brief Constructor.
     */
    ClientSessionSendThread(Server* server, sptk::LogEngine& logEngine)
        : ClientSessionThread(server, logEngine, sendSessionBatchSize())
    {
    }

    /**
     * @brief Destructor.
     */
    ~ClientSessionSendThread() override = default;

protected:
    void processSession(SClientSession& clientSession) override;

    /**
     * @brief Writes every session's queued bytes, one session at a time through its buffer's
     *        send().
     */
    void processBatch(std::span<SClientSession> sessions) override;

private:
    std::unique_ptr<sptk::Buffer> m_exchangeBuffer = std::make_unique<sptk::Buffer>();

    /// One spare buffer per session in a round. Reused between rounds, never shrunk.
    std::vector<std::unique_ptr<sptk::Buffer>> m_exchangeBuffers;
};

using SClientSessionSendThread = std::shared_ptr<ClientSessionSendThread>;

} // namespace xmq
