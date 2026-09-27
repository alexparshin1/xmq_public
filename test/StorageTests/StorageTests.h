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

#include "TestStorage.h"
#include "base/xmq.h"
#include "test/ServerTests_Suite.h"
#include "test/TestServers.h"

#include <gtest/gtest.h>

namespace xmq {

class XMQ_EXPORT XMQ_StorageTests
    : public ServerTests_Suite
{
public:
    /**
     * @brief Constructor.
     */
    XMQ_StorageTests() = default;

    XMQ_StorageTests(const XMQ_StorageTests&) = delete;
    XMQ_StorageTests(XMQ_StorageTests&&) = delete;
    XMQ_StorageTests& operator=(const XMQ_StorageTests&) = delete;
    XMQ_StorageTests& operator=(XMQ_StorageTests&&) = delete;

    /**
     * @brief Destructor.
     */
    ~XMQ_StorageTests() override = default;

    /**
     * @brief Execute before each test.
     */
    static void testPersistentSubscription(bool cleanSession);
    static void testPersistentSubscriptionPerformance(bool cleanSession);

    static void sessionPersistenceTest(bool cleanSession);

    /**
     * @brief Print storage info.
     * @param connectString     Connect string.
     */
    static void printStorageInfo(const std::string& connectString);

    static constexpr auto MaxStorageThreads = 4;

    static std::vector<SMessageDelivery> createTestMessageDeliveries(size_t count, const Server* server, const TestStorage* storage, size_t messageSize = 16, const SClientSession& clientSession = {});
    static SMessageDelivery              createTestMessageDelivery(const Server* server, const SClientSession& clientSession, std::string_view payload, const std::function<void(const std::shared_ptr<MessageDelivery>&)>& completionCallback);

private:
    static void linkStorageQueueTests();
    static void linkMessageTests();
};

} // namespace xmq
