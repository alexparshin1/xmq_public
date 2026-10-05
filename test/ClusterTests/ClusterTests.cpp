/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "test/ClusterTests/ClusterTests.h"
#include "test/ClusterTests/TestCluster.h"

#include "TestOptions.h"
#include "common/DirectoryNames.h"
#include "server/Settings/Settings.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

const Host XMQ_ClusterTests::m_primaryServerHost("localhost", 1880);
const Host XMQ_ClusterTests::m_secondaryServerHost("localhost", 1886);

void XMQ_ServerTestsLink::linkClusterTests()
{
    // Force linking this module
}

void XMQ_ClusterTests::SetUp()
{
    printTitle(testName());
}

void XMQ_ClusterTests::TearDown()
{
    stopServers();
    distrustNodeCertificates();
    XMQ_ServerTests::TearDown();
}

SServer XMQ_ClusterTests::createNode(const std::string& nodeName, const uint16_t              portNumber,
                                     const bool         cleanStart, const vector<LogSubject>& logSubjects)
{
    using enum LogPriority;
    // Cluster tests exercise cluster links, not the separately configured legacy bridges.
    SServer server = createServer(portNumber, static_cast<uint16_t>(portNumber + 7000), 0, cleanStart, nodeName, "cluster",
                                  "cluster", Debug, true, false);

    server->getSettings()->setLogSubjectsPriority({}, Info);
    server->getSettings()->setLogSubjectsPriority(logSubjects, Debug);
    trustNodeCertificate(server);

    return server;
}

void XMQ_ClusterTests::trustNodeCertificate(const SServer& server)
{
    // Every node in this process serves the same certificate, so trusting it once lets each of
    // them verify the others - which a cluster link always does.
    const filesystem::path served = server->getSettings()->m_connections.m_ssl_keys.m_certfile.asString().c_str();
    const auto             peers = Settings::peerCertificatesDirectory();
    error_code             errorCode;
    filesystem::create_directories(peers, errorCode);
    filesystem::copy_file(served, peers / "cluster-test-node.crt", filesystem::copy_options::overwrite_existing, errorCode);
    ASSERT_FALSE(errorCode) << "Can't trust " << served.string() << ": " << errorCode.message();
}

void XMQ_ClusterTests::distrustNodeCertificates()
{
    // The peers directory belongs to the whole run; left behind, it would have the bridge tests
    // verify links they expect to open unverified.
    error_code errorCode;
    filesystem::remove_all(Settings::peerCertificatesDirectory(), errorCode);
    for (const auto& entry: filesystem::directory_iterator(DirectoryNames::certsDirectory(), errorCode))
    {
        if (entry.path().filename().string().starts_with("peers-") && entry.path().extension() == ".crt")
        {
            filesystem::remove(entry.path(), errorCode);
        }
    }
}

tuple<client::SMqttClient, client::SMqttClient, std::string>
XMQ_ClusterTests::createTestSubscriberAndPublisher(const Host& publishToHost, const Host& subscribeToHost)
{
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    ConnectCredentials credentials(subscriberClientId, "user", "secret");

    auto subscriber = make_shared<client::MqttClient>(logEngine());
    auto rc = subscriber->connect(subscribeToHost, credentials, {.m_cleanSession = false}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(subscriber->isConnected());
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions(Qos::Qos1)));

    ConnectCredentials credentials2(publisherClientId, "user", "secret");
    auto               publisher = make_shared<client::MqttClient>(logEngine());
    rc = publisher->connect(publishToHost, credentials2, {.m_cleanSession = true}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(publisher->isConnected());

    return {publisher, subscriber, topicName};
}

tuple<SServer, SServer> XMQ_ClusterTests::makeClusterOfTwoNodes()
{
    using enum LogSubject;
    const vector logSubjects{
        Connect,
        Disconnect,
        ClusterConnections,
        ClusterEvents};

    const auto primaryNode = createNode("primary", 1880, true, logSubjects);
    const auto secondaryNode = createNode("secondary", 1886, false, logSubjects);

    secondaryNode->attachToCluster(primaryNode->getCluster()->getNodeHost());

    // Until each node holds the link the other opened to it, rather than a fixed half second.
    const auto linked = [](const SServer& origin, const SServer& destination)
    {
        const auto session = destination->getClientSession(
            format("node_{}_{}", origin->getNodeName().c_str(), destination->getNodeName().c_str()));
        return session && session->getConnection();
    };
    EXPECT_TRUE(TestCluster::waitFor([&]
                                     {
                                         return linked(primaryNode, secondaryNode) && linked(secondaryNode, primaryNode);
                                     }))
        << "The two nodes did not link up";
    return {primaryNode, secondaryNode};
}
