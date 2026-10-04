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

    void SetUp() override;
    void TearDown() override;

protected:
    static SServer createNode(const std::string& nodeName, uint16_t                         portNumber,
                              bool               cleanStart, const std::vector<LogSubject>& logSubjects = {});

    /**
     * @brief Trust the certificate a node serves, as a cluster link requires.
     * @param server            Node whose certificate the other nodes are to trust.
     */
    static void trustNodeCertificate(const SServer& server);

    /**
     * @brief Trust no node: empty the peers directory.
     */
    static void distrustNodeCertificates();

    static std::tuple<client::SMqttClient, client::SMqttClient, std::string>
    createTestSubscriberAndPublisher(const sptk::Host& publishToHost, const sptk::Host& subscribeToHost);

    static std::tuple<SServer, SServer> makeClusterOfTwoNodes();
    static std::vector<SServer>         makeTestCluster(size_t nodeCount, uint16_t firstPortNumber);

    static int linkClusterTests();
};

} // namespace xmq