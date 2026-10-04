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

#pragma once

#include "client/MqttClient.h"
#include "server/Server.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xmq {

/**
 * @brief A cluster of test nodes, started, joined and fully meshed in one line.
 *
 *     TestCluster cluster(3);
 *     auto client = cluster.connect(1, "client-id");
 *     cluster.stopNode(2);
 *     cluster.startNode(2);
 *
 * Node i is named node_0i, listens for MQTT on FirstPort + i and for MQTT+SSL on FirstPort + i +
 * 7000, and trusts the others' certificate, as a cluster link requires. The constructor and every
 * node start return only once each running node has a link to every other, or fail the test.
 * Nodes are stopped when the cluster is destroyed.
 */
class TestCluster
{
public:
    static constexpr uint16_t FirstPort = 1890; ///< Not 180x: 1803 is taken by JetBrains Toolbox.
    static constexpr size_t   MaxNodes = 10;    ///< Ports run to FirstPort + MaxNodes - 1.

    /**
     * @brief Start the nodes and join them into one cluster.
     * @param nodeCount         Number of nodes, up to MaxNodes.
     * @param logSubjects       Log subjects to log at debug level on every node.
     */
    explicit TestCluster(size_t nodeCount, std::vector<LogSubject> logSubjects = {LogSubject::ClusterConnections,
                                                                                    LogSubject::ClusterEvents});

    ~TestCluster();

    TestCluster(const TestCluster&) = delete;
    TestCluster& operator=(const TestCluster&) = delete;

    /**
     * @return Number of nodes, running or stopped.
     */
    [[nodiscard]] size_t size() const
    {
        return m_nodes.size();
    }

    /**
     * @param index             Node index.
     * @return the node's server, or null while the node is stopped.
     */
    [[nodiscard]] const SServer& operator[](size_t index) const;

    /**
     * @param index             Node index.
     * @return the node's name.
     */
    [[nodiscard]] static std::string nodeName(size_t index);

    /**
     * @param index             Node index.
     * @return the node's plain MQTT address, which test clients connect to.
     */
    [[nodiscard]] static sptk::Host host(size_t index);

    /**
     * @brief Connect a test client, as user/secret, to a node.
     * @param index             Node index.
     * @param clientId          Client ID.
     * @param cleanSession      Clean session flag.
     * @return the connected client; the test fails if it could not connect.
     */
    [[nodiscard]] client::SMqttClient connect(size_t index, const std::string& clientId, bool cleanSession = true) const;

    /**
     * @brief Start one more node and join it to the cluster.
     * @return the new node's index.
     */
    size_t addNode();

    /**
     * @brief Stop a node. Its persistent state stays in storage.
     * @param index             Node index.
     */
    void stopNode(size_t index);

    /**
     * @brief Start a stopped node again with the state it kept, and rejoin it to the cluster.
     * @param index             Node index.
     */
    void startNode(size_t index);

    /**
     * @brief Wait until every running node has a link to every other running node.
     * @param timeout           How long to wait.
     * @return true if the mesh is complete.
     */
    [[nodiscard]] bool waitForMesh(std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

    /**
     * @param index             Node index; the node must be running.
     * @param topic             Concrete topic.
     * @return the retained payload the node holds for the topic, or nothing.
     */
    [[nodiscard]] std::optional<std::string> retained(size_t index, const std::string& topic) const;

    /**
     * @brief Wait for a condition, checking it every 20 ms.
     * @param condition         Condition to wait for.
     * @param timeout           How long to wait.
     * @return the condition's last value.
     */
    static bool waitFor(const std::function<bool()>& condition,
                        std::chrono::milliseconds    timeout = std::chrono::seconds(2));

private:
    std::vector<SServer>    m_nodes;       ///< Node servers; null while stopped.
    std::vector<LogSubject> m_logSubjects; ///< Logged at debug level.

    /**
     * @brief Start a node and join it to the first other running node.
     * @param index             Node index.
     * @param cleanStart        Start from empty storage.
     */
    void start(size_t index, bool cleanStart);
};

} // namespace xmq
