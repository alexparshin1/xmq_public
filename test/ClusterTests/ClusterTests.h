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

class XMQ_EXPORT XMQ_ClusterTests : public XMQ_ServerTests
{
public:
    static const sptk::Host m_primaryServerHost;
    static const sptk::Host m_secondaryServerHost;

    static constexpr int TestLeaseSeconds = 2; ///< cluster.lease_seconds of every test node.

    void SetUp() override;
    void TearDown() override;

    /**
     * @brief Delete what the last test's cluster left in the shared storage.
     */
    static void clearClusterState();

    /**
     * @brief Start a cluster node: MQTT on portNumber, MQTT+SSL on portNumber + 7000.
     *
     * Most tests want a TestCluster instead, which starts and joins the nodes as well.
     */
    static SServer createNode(const std::string& nodeName, uint16_t                         portNumber,
                              bool               cleanStart, const std::vector<LogSubject>& logSubjects = {});

    /**
     * @brief Stop one node started with createNode(), leaving the others running.
     * @param nodeName          Node name.
     */
    static void stopNode(const std::string& nodeName)
    {
        stopServer(nodeName);
    }

    /**
     * @brief Trust the certificate a node serves, as a cluster link requires.
     * @param server            Node whose certificate the other nodes are to trust.
     */
    static void trustNodeCertificate(const SServer& server);

    /**
     * @brief Trust no node: empty the peers directory.
     */
    static void distrustNodeCertificates();

protected:
    static std::tuple<client::SMqttClient, client::SMqttClient, std::string>
    createTestSubscriberAndPublisher(const sptk::Host& publishToHost, const sptk::Host& subscribeToHost);

    static std::tuple<SServer, SServer> makeClusterOfTwoNodes();

    static int linkClusterTests();
};

} // namespace xmq