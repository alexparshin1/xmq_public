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

#include "server/Server.h"
#include <gtest/gtest.h>
#include <sptk5/cutils>

class XMQ_EXPORT XMQ_MessageReaderTests : public testing::Test
{
public:
    static constexpr auto    m_tinyTimeout {std::chrono::milliseconds(10)};
    static constexpr auto    m_smallTimeout {std::chrono::milliseconds(100)};
    static constexpr uint8_t m_commandStartMarker {0xFF};

    /**
     * @brief Constructor.
     */
    XMQ_MessageReaderTests() = default;
    /**
     * @brief Destructor.
     */
    ~XMQ_MessageReaderTests() override = default;

    /**
     * @brief Execute before each test.
     */
    void SetUp() override;

    void stop()
    {
        if (m_server)
        {
            m_server->stop();
            m_server.reset();

            m_clientSocket->close();
            m_clientSocket.reset();
        }
    }

    [[nodiscard]] std::shared_ptr<sptk::FastTCPServer> server() const
    {
        return m_server;
    }

    static void messageReaderTestFunction(const sptk::ServerConnection& serverConnection);

    std::shared_ptr<sptk::TCPSocket> clientSocket() const;

private:
    static const uint16_t                m_portNumber;
    std::shared_ptr<sptk::FastTCPServer> m_server;
    void                                 createServer();
    std::shared_ptr<sptk::TCPSocket>     m_clientSocket;
};
