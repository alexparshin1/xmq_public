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

#include "test/ClusterTests/TestCluster.h"
#include "test/ClusterTests/ClusterTests.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
constexpr uint16_t SslPortOffset = 7000; ///< The MQTT+SSL listener's distance from the MQTT one.
}

TestCluster::TestCluster(const size_t nodeCount, vector<LogSubject> logSubjects)
    : m_logSubjects(std::move(logSubjects))
{
    if (nodeCount == 0 || nodeCount > MaxNodes)
    {
        throw Exception(format("A test cluster has 1 to {} nodes, not {}", MaxNodes, nodeCount));
    }
    for (size_t index = 0; index < nodeCount; ++index)
    {
        addNode();
    }
}

TestCluster::~TestCluster()
{
    for (size_t index = 0; index < m_nodes.size(); ++index)
    {
        if (m_nodes[index])
        {
            XMQ_ClusterTests::stopNode(nodeName(index));
        }
    }
}

const SServer& TestCluster::operator[](const size_t index) const
{
    return m_nodes.at(index);
}

string TestCluster::nodeName(const size_t index)
{
    return format("node_{:02d}", index);
}

Host TestCluster::host(const size_t index)
{
    return {"localhost", static_cast<uint16_t>(FirstPort + index)};
}

client::SMqttClient TestCluster::connect(const size_t index, const string& clientId, const bool cleanSession) const
{
    auto client = make_shared<client::MqttClient>(ServerTests_Suite::logEngine());
    const auto rc = client->connect(host(index), ConnectCredentials(clientId, "user", "secret"),
                                    {.m_cleanSession = cleanSession}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc) << clientId << " could not connect to " << nodeName(index);
    return client;
}

size_t TestCluster::addNode()
{
    if (m_nodes.size() == MaxNodes)
    {
        throw Exception(format("A test cluster has at most {} nodes", MaxNodes));
    }
    m_nodes.emplace_back();
    const auto index = m_nodes.size() - 1;
    start(index, true);
    return index;
}

void TestCluster::stopNode(const size_t index)
{
    if (m_nodes.at(index))
    {
        XMQ_ClusterTests::stopNode(nodeName(index));
        m_nodes[index].reset();
    }
}

void TestCluster::startNode(const size_t index)
{
    if (!m_nodes.at(index))
    {
        start(index, false);
    }
}

void TestCluster::start(const size_t index, const bool cleanStart)
{
    const auto port = host(index).port();
    m_nodes[index] = XMQ_ClusterTests::createNode(nodeName(index), port, cleanStart, m_logSubjects);

    // Joined through any other running node: once the cluster has formed, each one knows them all.
    for (size_t other = 0; other < m_nodes.size(); ++other)
    {
        if (other != index && m_nodes[other])
        {
            m_nodes[index]->attachToCluster(Host("localhost", static_cast<uint16_t>(host(other).port() + SslPortOffset)));
            break;
        }
    }

    if (!waitForMesh())
    {
        throw Exception(format("{} did not form a full mesh with the other nodes", nodeName(index)));
    }
}

bool TestCluster::waitForMesh(const chrono::milliseconds timeout) const
{
    // Complete when, for every ordered pair of running nodes, the destination holds the session of
    // the link the origin opened to it.
    const auto meshed = [this]
    {
        for (size_t origin = 0; origin < m_nodes.size(); ++origin)
        {
            for (size_t destination = 0; destination < m_nodes.size(); ++destination)
            {
                if (origin == destination || !m_nodes[origin] || !m_nodes[destination])
                {
                    continue;
                }
                const auto session = m_nodes[destination]->getClientSession(
                    format("node_{}_{}", nodeName(origin), nodeName(destination)));
                if (!session || !session->getConnection())
                {
                    return false;
                }
            }
        }
        return true;
    };
    return waitFor(meshed, timeout);
}

optional<string> TestCluster::retained(const size_t index, const string& topic) const
{
    optional<string> payload;
    m_nodes.at(index)->getSubscriptionManager()->retainedMessages().forEachMatching(
        topic, [&payload](const string&, const RetainedMessages::Record& record)
        {
            payload = record.m_payload;
        });
    return payload;
}

bool TestCluster::waitFor(const function<bool()>& condition, const chrono::milliseconds timeout)
{
    const auto deadline = chrono::steady_clock::now() + timeout;
    while (!condition())
    {
        if (chrono::steady_clock::now() >= deadline)
        {
            return condition();
        }
        this_thread::sleep_for(20ms);
    }
    return true;
}
