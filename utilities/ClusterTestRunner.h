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

#include "ClusterTestCommandLine.h"
#include "ClusterTestDefinition.h"
#include "Utility.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace xmq {

/**
 * @brief Cluster testing utility: a load scenario, with the cluster changed under it.
 *
 * xmq_scn runs a scenario and the scenario is the whole run: it starts, it ends, and nothing happens
 * to the cluster in between. What a cluster has to be tested for is the opposite of that - a node
 * taken away while clients are connected, a node brought back into a cluster that moved on without
 * it, storage lost and found again - and none of it can be written as a scenario, because a scenario
 * cannot stop a broker.
 *
 * So this runs two things at once. The load is an ordinary scenario, run exactly as xmq_scn runs it,
 * in a thread of its own; the timeline is a list of moments at which this process stops a node,
 * starts it again, or looks at what the cluster believes. What is left afterwards is the checks: a
 * query in the shared storage, or a client's own view of the cluster, both of which have to hold
 * when the dust settles.
 */
class ClusterTestRunner final : public Utility
{
public:
    /**
     * @brief Constructor.
     * @param args Command line arguments.
     */
    explicit ClusterTestRunner(const std::vector<std::string>& args);

    /**
     * @brief Run the test: the load, the timeline, and the checks.
     * @return Exit code: zero when every check passed.
     */
    int run() override;

private:
    /**
     * @brief What a check answered, kept for the summary at the end.
     */
    struct CCheckOutcome
    {
        std::string m_name;
        bool        m_passed {false};
        std::string m_detail; ///< What was expected and what was there.
    };

    ClusterTestDefinition m_test;
    std::vector<CCheckOutcome> m_outcomes;
    std::vector<bool>          m_ran; ///< One per check of the test, in its order.
    size_t                     m_failedSteps {0}; ///< Timeline steps whose command exited non-zero.
    /// Numbers the client ids of the checks apart; see checkClientId().
    mutable std::atomic<uint64_t> m_checkClientSequence {0};
    std::chrono::steady_clock::time_point m_loadStarted;

    [[nodiscard]] const ClusterTestCommandLine& arguments() const;

    void loadTestFile(const std::filesystem::path& file);
    void applyCommandLineOverrides();
    void printPlan() const;

    /**
     * @brief Run the load scenario, to the end. Called on its own thread.
     */
    void runLoad();

    void runTimeline();
    void waitUntil(std::chrono::seconds at) const;
    void stopOrStartNode(const CClusterTimelineStep& step);

    /**
     * @brief Run the named check, or every check that has not run yet.
     */
    void runChecks(const std::string& only = {});

    /**
     * @brief Run one check.
     * @return True when it passed. @param detail What was expected and what was there.
     */
    bool runCheck(const CClusterCheck& check, std::string& detail);
    bool checkRedis(const CClusterCheck& check, std::string& detail);
    bool checkConnect(const CClusterCheck& check, std::string& detail);
    bool checkDeliver(const CClusterCheck& check, std::string& detail);
    bool checkRetained(const CClusterCheck& check, std::string& detail);

    /**
     * @brief Carry one message across the cluster, and see that it arrives.
     *
     * One implementation for both kinds, because they differ in one thing only: a retained message is
     * published before anyone subscribes to it, which is what makes it a retained message.
     *
     * @param check   The check, naming the two ends, the topic and the payload.
     * @param retain  Publish it retained, and clear it afterwards.
     * @param detail  What arrived where, or why nothing did.
     * @return True when the message arrived where the check says it had to.
     */
    bool carryMessage(const CClusterCheck& check, bool retain, std::string& detail);

    /**
     * @brief Print the summary and say whether the test passed.
     * @return Exit code: zero when every check passed.
     */
    [[nodiscard]] int report() const;

    /// Where a check talks: the node it names, or the host and port it spells out, or the test's own
    /// server when it names neither. The node wins, so a test that gives its nodes addresses can be
    /// pointed at another stand by editing the nodes and nothing else.
    struct CAddress
    {
        std::string m_host;
        uint16_t    m_port {0};
    };

    [[nodiscard]] CAddress addressFor(const std::string& nodeName, const std::string& host,
                                      uint16_t port) const;
    [[nodiscard]] ConnectCredentials credentials(std::string_view clientId) const;

    /**
     * @brief A client id of this check's own, used by no other client of the run.
     *
     * A check that reused an id would not be checking the cluster but asking it to move a session:
     * the id would be held by whichever node the previous user of it connected to, and a node that
     * meets a session another node owns either takes it over or refuses. The first run of this
     * against six nodes found exactly that - the checks shared the load scenario's ids, and theater
     * answered "The MQTT server unavailable" for a session another machine held.
     */
    [[nodiscard]] std::string checkClientId(std::string_view checkName) const;

    /**
     * @brief Run a command and take what it printed, for a check that has to be told something.
     *
     * A shell command, because what is asked is as likely to be a web service of a node as an ssh
     * to another machine, and the tool already reaches the nodes that way.
     */
    [[nodiscard]] std::string captureCommand(const std::string& command) const;

    /**
     * @brief The protocol the checks speak.
     *
     * MQTT 5 unless the command line asked for another: the cluster is 5 territory - session
     * takeover, reason codes, retained handling - and a check is meant to see what a client of the
     * cluster sees. The load's own clients take their protocol from the scenario file, as always.
     */
    [[nodiscard]] ProtocolVersion checkProtocolVersion() const;
};

} // namespace xmq
