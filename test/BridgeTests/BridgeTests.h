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

#include "client/MqttClient.h"
#include "test/ServerTests/ServerTests.h"

namespace xmq {

class XMQ_EXPORT XMQ_BridgeTests : public XMQ_ServerTests
{
public:
    static const sptk::Host m_xmqServerHost;
    static const sptk::Host m_otherServerHost;

    void SetUp() override;

    /// This is the suite the configured bridges exist for.
    [[nodiscard]] bool bridgesNeeded() const override
    {
        return true;
    }

    /**
     * @brief Block until the bridging node's bridges are connected.
     *
     * A bridge subscribes on the remote broker only once connected, and forwards local traffic
     * only once its own subscription exists, so anything published beforehand is not carried.
     * The bridging node also starts before its peer is listening, so its first connection attempt
     * always fails and is retried.
     */
    static void waitForBridge();

protected:
    static std::tuple<client::SMqttClient, client::SMqttClient, std::string>
    createTestSubscriberAndPublisher(const sptk::Host& publishToHost, const sptk::Host& subscribeToHost);

    static int linkClusterTests();
};

} // namespace xmq
