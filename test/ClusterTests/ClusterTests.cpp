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

#include "TestOptions.h"

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
    XMQ_ServerTests::TearDown();
}

SServer XMQ_ClusterTests::createNode(const std::string& nodeName, const uint16_t              portNumber,
                                     const bool         cleanStart, const vector<LogSubject>& logSubjects)
{
    using enum LogPriority;
    // The cluster is built out of the bridges in the test configuration, so this suite asks for
    // them; the last two arguments are persistence, left at its default, and that request.
    SServer server = createServer(portNumber, 0, 0, cleanStart, nodeName, "cluster",
                                  "cluster", Debug, true, true);

    server->getSettings()->setLogSubjectsPriority({}, Info);
    server->getSettings()->setLogSubjectsPriority(logSubjects, Debug);

    this_thread::sleep_for(100ms);
    return server;
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

    string databaseUri = "postgresql://localhost/xmq_test";

    const auto primaryNode = createNode("primary", 1880, true, logSubjects);
    const auto secondaryNode = createNode("secondary", 1886, false, logSubjects);

    secondaryNode->attachToCluster(Host("localhost", 1880));
    this_thread::sleep_for(500ms);
    return {primaryNode, secondaryNode};
}

vector<SServer> XMQ_ClusterTests::makeTestCluster(const size_t nodeCount, const uint16_t firstPortNumber)
{
    using enum LogSubject;
    const vector logSubjects{
        Connect,
        Disconnect,
        ClusterConnections,
        ClusterEvents,
    };

    unique_ptr<Host> primaryHost;
    vector<SServer>  nodes;
    for (auto i = 0u; i < nodeCount; ++i)
    {
        const auto portNumber = static_cast<uint16_t>(firstPortNumber + i);
        const auto nodeName = format("node_{:02d}", i);

        // Every node starts clean, not just the first one. A clean start drops the starting node's
        // own state and nothing else (RedisStorage::clear), so leaving the later nodes dirty left
        // their persistent subscriber sessions - and the messages queued to them - in Redis, to be
        // redelivered on the next run.
        const auto node = createNode(nodeName, portNumber, true, logSubjects);
        if (i == 0)
        {
            // The joining nodes have to dial this address, so it must be a connectable one.
            // listenerHosts() reports the bind address instead - 0.0.0.0 - which Linux accepts as
            // a connect target (it falls back to the local host) but Windows rejects outright.
            primaryHost = make_unique<Host>("localhost", portNumber);
        }
        else
        {
            node->attachToCluster(*primaryHost);
        }

        nodes.push_back(node);

        this_thread::sleep_for(100ms);
    }
    return nodes;
}