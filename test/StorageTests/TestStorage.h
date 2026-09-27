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

#include "../../server/ClientSession/ClientSessionThreadManager.h"
#include "common/DirectoryNames.h"
#include "server/Server.h"
#include <memory>
#include <sptk5/cutils>

namespace xmq {

class XMQ_EXPORT TestStorage
{
public:
    /**
     * @brief Constructor.
     */
    TestStorage(const SServer& server, bool cleanSession = true);

    TestStorage(const TestStorage&) = delete;
    TestStorage(TestStorage&&) = delete;
    TestStorage& operator=(const TestStorage&) = delete;
    TestStorage& operator=(TestStorage&&) = delete;

    /**
     * @brief Destructor.
     */
    ~TestStorage();

    size_t countSessionSubscriptions(const std::string& topicName, const SClientSession& session = {}) const;

    void createDefaultSession(bool cleanSession);
    void releaseDefaultSession();

    SStorage storage() const
    {
        return m_storage;
    }

    void connect() const;

    void disconnect() const
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_storage->disconnect();
    }

    SClientSession session() const
    {
        return m_session;
    }

private:
    SServer              m_server;
    SStorage             m_storage;
    sptk::FileLogEngine  m_logEngine {"TestStorage.log"};
    ClientSessionThreads m_nullThreads;
    SClientSession       m_session;

    void loadSubscriptions();
};

} // namespace xmq
