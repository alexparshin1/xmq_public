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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xmq {

/**
 * @brief One step of a cluster test's timeline: something done to the cluster while the load runs.
 *
 * Steps are timed from the start of the load, not from each other: a test that says a node goes away
 * at 60s means 60s into the load, whatever moved earlier in the file.
 */
struct CClusterTimelineStep
{
    enum class Action : uint8_t
    {
        StopNode,  ///< Take a node away, the way a crash or a restart takes it.
        StartNode, ///< Bring it back.
        Checks     ///< Run the named check, or every check that has not run yet.
    };

    Action               m_action {Action::Checks};
    std::chrono::seconds m_at {0};
    std::string          m_node;  ///< StopNode and StartNode.
    std::string          m_check; ///< Checks: by name, or empty for all outstanding ones.
};

/**
 * @brief A node of the cluster under test: where it listens, and how to stop and start it.
 *
 * The commands are shell commands, spelled out in the test file, because this orchestrator does not
 * own the nodes: a stand runs them as processes, the farm runs them as a service, and one that
 * hardcoded either would be good on one machine only. `{node}` in a command stands for the node's
 * name, so a stand that takes one argument per node needs it written once.
 *
 * The address is here so that the rest of the file can name a node rather than repeat its port in
 * every check and in the load: one fact, one place, and the test moves to another stand by writing
 * three fields.
 */
struct CClusterNode
{
    std::string m_name;
    std::string m_host;  ///< Where the node listens for clients.
    uint16_t    m_port {0};
    std::string m_stopCommand;
    std::string m_startCommand;
};

/**
 * @brief One thing that has to be true of the cluster, checked while or after the load runs.
 *
 * Four kinds, and the two groups answer different questions. A query in the shared storage asks what
 * the cluster believes about itself, which MQTT cannot show - who is a member, who holds a session,
 * whether a lease was given up. A client's view asks what a client would see, which the storage
 * cannot show - that the two agree is the point of the test.
 */
struct CClusterCheck
{
    enum class Kind : uint8_t
    {
        Redis,       ///< A command in the shared storage; the answer is compared as text.
        MqttConnect, ///< A client connects to a node, or fails to.
        MqttDeliver, ///< A message published on one node arrives on another.
        MqttRetained ///< A retained message published on one node is read on another.
    };

    std::string m_name;
    Kind        m_kind {Kind::Redis};

    /// Redis. The answer of a command that returns several values is those values joined with ", ",
    /// so a check that wants a count asks a command that returns one - ZCARD rather than KEYS.
    std::string m_uri;
    std::string m_command;
    std::string m_expect;   ///< The answer, whole.
    std::string m_contains; ///< Instead of the whole answer, a part of it.
    long long   m_min {std::numeric_limits<long long>::min()}; ///< Instead: the answer as a number.
    bool        m_nonEmpty {false};
    bool        m_empty {false};

    /// MQTT. A check names the node it talks to - by name where the test gives the node an address,
    /// which is the shorter and the only form that survives the stand moving; the host and port
    /// remain for a check that talks to something which is not one of the cluster's nodes.
    std::string          m_node;                 ///< Instead of m_host and m_port.
    std::string          m_host;
    uint16_t             m_port {1883};
    std::string          m_publishNode;          ///< Instead of m_publishHost and m_publishPort.
    std::string          m_publishHost;
    uint16_t             m_publishPort {1883};
    std::string          m_subscribeNode;        ///< Instead of m_subscribeHost and m_subscribePort.
    std::string          m_subscribeHost;
    uint16_t             m_subscribePort {1883};
    std::string          m_topic;
    std::string          m_payload;
    std::chrono::seconds m_timeout {10};
    bool                 m_expectConnected {true};
};

/**
 * @brief A cluster test: the nodes, what happens to them, the load that runs while it does, and what
 * has to be true when it is over.
 *
 * Read from a JSON file, which names an ordinary load scenario rather than containing one: the load
 * is what the release measures and is written and reviewed on its own, while this file is about the
 * cluster - which node goes away at which second, and what the cluster has to do about it.
 */
class ClusterTestDefinition final
{
public:
    /**
     * @brief Read the test file.
     * @param testFile  Path to the test file.
     */
    void load(const std::filesystem::path& testFile);

    /**
     * @brief The node of that name.
     * @throws sptk::Exception  When the test has no such node.
     */
    [[nodiscard]] const CClusterNode& node(std::string_view name) const;

    /**
     * @brief The check of that name.
     * @throws sptk::Exception  When the test has no such check.
     */
    [[nodiscard]] const CClusterCheck& check(std::string_view name) const;

    std::string           m_name;
    std::string           m_serverHost;         ///< Where the load connects unless it says otherwise.
    uint16_t              m_serverPort {1883};
    std::string           m_username;           ///< And the account it connects with.
    std::string           m_password;
    std::filesystem::path m_loadScenario;       ///< The scenario to run, made absolute on loading.
    /// Which node of the cluster each group of the load runs against: publishers, subscribers.
    /// Empty where the scenario itself says where its clients go.
    std::map<std::string, std::string> m_loadGroups;
    std::vector<CClusterNode>         m_nodes;
    std::vector<CClusterTimelineStep> m_timeline; ///< In the order it happens, whatever the file says.
    std::vector<CClusterCheck>        m_checks;
};

} // namespace xmq
