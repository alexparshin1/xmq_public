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
     * @brief Detaches every session's buffer, then writes the whole round through one writer.
     */
    void processBatch(std::span<SClientSession> sessions) override;

private:
    std::unique_ptr<sptk::Buffer> m_exchangeBuffer = std::make_unique<sptk::Buffer>();

    /// One spare buffer per session in a round: the whole batch is detached before anything is
    /// written, so they are all outstanding at once. Reused between rounds, never shrunk.
    std::vector<std::unique_ptr<sptk::Buffer>> m_exchangeBuffers;

    /// Reused between rounds so a round costs no allocation.
    std::vector<SocketWriteItem> m_writeItems;

    /// Parallel to m_writeItems: which session each write belongs to, needed to close the
    /// right one when a write fails.
    std::vector<SClientSession> m_sessionOfItem;

    /// Parallel to m_writeItems as well: the connection each write went out on. Closing a session
    /// without naming it skips the comparison that keeps a failed write on an old connection from
    /// tearing down the one that has replaced it.
    std::vector<std::shared_ptr<ServerConnectionExt>> m_connectionOfItem;

    UBatchSocketWriter m_writer {createBatchSocketWriter()};
};

using SClientSessionSendThread = std::shared_ptr<ClientSessionSendThread>;

} // namespace xmq
