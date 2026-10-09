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

#include "ClusterTestRunner.h"

#include "ClusterTestCommandLine.h"
#include "Scenario.h"
#include "scenario/ScenarioEngine.h"

#include <sptk5/net/Host.h>
#include <sptk5/net/RedisCommand.h>
#include <sptk5/net/RedisConnect.h>
#include <sptk5/net/URL.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/** @brief The words of a command as it goes to Redis: spaces separate them, and nothing else is special. */
vector<string> words(const string& command)
{
    vector<string> result;
    istringstream  stream(command);
    string         word;
    while (stream >> word)
    {
        result.push_back(word);
    }
    return result;
}

string trimmed(const string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == string::npos)
    {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

/** @brief A moment of the run, in seconds from the start of the load. */
string stamp(const chrono::steady_clock::time_point start)
{
    const auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start);
    return format("+{:.1f}s", static_cast<double>(elapsed.count()) / 1000.0);
}

/** @brief What a check of kind Redis expects and what it found, in the same words. */
string expectation(const CClusterCheck& check)
{
    if (!check.m_expect.empty())
    {
        return format("'{}'", check.m_expect);
    }
    if (!check.m_contains.empty())
    {
        return format("containing '{}'", check.m_contains);
    }
    if (check.m_nonEmpty)
    {
        return "not empty";
    }
    if (check.m_empty)
    {
        return "empty";
    }
    return format("at least {}", check.m_min);
}

} // namespace

ClusterTestRunner::ClusterTestRunner(const vector<string>& args)
    : Utility(make_shared<ClusterTestCommandLine>(args))
{
}

const ClusterTestCommandLine& ClusterTestRunner::arguments() const
{
    return dynamic_cast<const ClusterTestCommandLine&>(commandLine());
}

int ClusterTestRunner::run()
{
    const auto scenarioOption = commandLine().getOptionValue("scenario");
    if (scenarioOption.empty())
    {
        throw Exception("A cluster test file is required: use --scenario.");
    }

    const filesystem::path file(scenarioOption.c_str());
    if (!filesystem::exists(file))
    {
        throw Exception(format("No such cluster test file: {}.", file.string()));
    }

    loadTestFile(file);
    applyCommandLineOverrides();
    printPlan();

    if (commandLine().hasOption("dry-run"))
    {
        return 0;
    }

    // The load runs on a thread of its own while the timeline changes the cluster under it. What it
    // throws is kept and reported with everything else: a load that ends in an error when its node is
    // taken away is a result about the cluster, not a failure of this tool.
    exception_ptr loadFailure;
    m_loadStarted = chrono::steady_clock::now();
    thread loadThread(
        [this, &loadFailure]
        {
            try
            {
                runLoad();
            }
            catch (...)
            {
                loadFailure = current_exception();
            }
        });

    runTimeline();
    loadThread.join();

    if (loadFailure)
    {
        try
        {
            rethrow_exception(loadFailure);
        }
        catch (const exception& e)
        {
            CERR(format("The load ended with an error: {}", e.what()));
        }
    }

    runChecks();
    return report();
}

void ClusterTestRunner::loadTestFile(const filesystem::path& file)
{
    m_test.load(file);

    if (const auto loadScenario = commandLine().getOptionValue("load-scenario"); !loadScenario.empty())
    {
        filesystem::path replacement(loadScenario.c_str());
        if (replacement.is_relative())
        {
            // Relative to the test file, as the one it replaces is: both name a scenario the same way.
            replacement = file.parent_path() / replacement;
        }
        m_test.m_loadScenario = replacement;
    }

    if (!filesystem::exists(m_test.m_loadScenario))
    {
        throw Exception(format("The load scenario does not exist: {}.", m_test.m_loadScenario.string()));
    }

    m_ran.assign(m_test.m_checks.size(), false);
}

void ClusterTestRunner::applyCommandLineOverrides()
{
    const auto& args = arguments();

    if (args.optionSpecified("host"))
    {
        m_test.m_serverHost = args.getOptionValue("host").c_str();
    }
    if (args.optionSpecified("port"))
    {
        m_test.m_serverPort = static_cast<uint16_t>(args.getOptionValue("port").toInt());
    }
    if (args.optionSpecified("username"))
    {
        m_test.m_username = args.getOptionValue("username").c_str();
    }
    if (args.optionSpecified("password"))
    {
        m_test.m_password = args.getOptionValue("password").c_str();
    }

    // A test that names no server at all takes what the command line would have used - localhost and
    // the MQTT port - so that one file works against a stand and against a broker reached by an
    // address given on the command line.
    if (m_test.m_serverHost.empty())
    {
        m_test.m_serverHost = commandLine().getOptionValue("host").c_str();
        m_test.m_serverPort = static_cast<uint16_t>(commandLine().getOptionValue("port").toInt());
    }
}

void ClusterTestRunner::printPlan() const
{
    COUT(format("Cluster test: {}", m_test.m_name));
    COUT(format("Load:         {}", m_test.m_loadScenario.string()));

    Strings groupBindings;
    for (const auto& [groupName, nodeName] : m_test.m_loadGroups)
    {
        groupBindings.push_back(format("{} -> {}", groupName, nodeName).c_str());
    }
    COUT(format("Load groups:  {}", groupBindings.empty() ? "as the scenario says" : groupBindings.join(", ").c_str()));
    COUT(format("Account:      '{}'", m_test.m_username));

    Strings nodeNames;
    for (const auto& node : m_test.m_nodes)
    {
        nodeNames.push_back(format("{} ({}:{})", node.m_name, node.m_host, node.m_port).c_str());
    }
    COUT(format("Nodes:        {}", nodeNames.join(", ").c_str()));

    COUT("");
    COUT("Timeline:");
    if (m_test.m_timeline.empty())
    {
        COUT("  (nothing: the load runs and the checks run after it)");
    }
    for (const auto& step : m_test.m_timeline)
    {
        const auto at = format("+{}s", step.m_at.count());
        switch (step.m_action)
        {
            case CClusterTimelineStep::Action::StopNode:
                COUT(format("  {:>6}  stop  {}", at, step.m_node));
                break;
            case CClusterTimelineStep::Action::StartNode:
                COUT(format("  {:>6}  start {}", at, step.m_node));
                break;
            case CClusterTimelineStep::Action::Checks:
                COUT(format("  {:>6}  check {}", at, step.m_check.empty() ? "all outstanding" : step.m_check));
                break;
        }
    }

    COUT("");
    COUT("Checks:");
    for (const auto& check : m_test.m_checks)
    {
        switch (check.m_kind)
        {
            case CClusterCheck::Kind::Redis:
                COUT(format("  {:<40} redis  {} ({})", check.m_name, check.m_command, expectation(check)));
                break;
            case CClusterCheck::Kind::MqttConnect:
            {
                const auto address = addressFor(check.m_node, check.m_host, check.m_port);
                COUT(format("  {:<40} mqtt   connect {}:{} ({})", check.m_name, address.m_host,
                            address.m_port, check.m_expectConnected ? "must connect" : "must not connect"));
                break;
            }
            case CClusterCheck::Kind::MqttDeliver:
            case CClusterCheck::Kind::MqttRetained:
            {
                const auto from = addressFor(check.m_publishNode, check.m_publishHost, check.m_publishPort);
                const auto to = addressFor(check.m_subscribeNode, check.m_subscribeHost, check.m_subscribePort);
                COUT(format("  {:<40} mqtt   {} on {}:{}, read on {}:{} ('{}')",
                            check.m_name,
                            check.m_kind == CClusterCheck::Kind::MqttRetained ? "retained" : "published",
                            from.m_host, from.m_port, to.m_host, to.m_port, check.m_topic));
                break;
            }
        }
    }
    COUT("");
}

void ClusterTestRunner::runLoad()
{
    ScenarioEngine engine;
    engine.logEngine(logEngine());
    engine.load(m_test.m_loadScenario);

    // The test's server, where it names one, is what the scenario's own server is replaced with: the
    // scenario is about clients and traffic and is written once, while which cluster it runs against
    // is what changes from test to test.
    auto& server = engine.scenario().m_server;
    if (!m_test.m_serverHost.empty())
    {
        server.m_hostname = String(m_test.m_serverHost.c_str());
        server.m_port = m_test.m_serverPort;
    }
    if (!m_test.m_username.empty())
    {
        server.m_username = String(m_test.m_username.c_str());
        server.m_password = String(m_test.m_password.c_str());
    }

    // A group the test points at a node takes that node's address. The scenario stays what it is -
    // clients and the traffic they make - while where its clients connect is a fact about the
    // cluster, and this test file is where the cluster is described.
    for (const auto& [groupName, nodeName] : m_test.m_loadGroups)
    {
        const auto& node = m_test.node(nodeName);
        auto*       group = groupName == "publishers" ? &engine.scenario().m_publishers
                                                     : &engine.scenario().m_subscribers;
        group->m_server.m_hostname = String(node.m_host.c_str());
        group->m_server.m_port = node.m_port;
        // A group that names no account takes the test's: the credentials belong to the cluster, the
        // scenario should not have to know them.
        if (group->m_server.m_username.asString().empty() && !m_test.m_username.empty())
        {
            group->m_server.m_username = String(m_test.m_username.c_str());
            group->m_server.m_password = String(m_test.m_password.c_str());
        }
    }

    engine.sslKeys(runDefinition().m_sslKeys);
    engine.commandProperties(runDefinition().m_commandConnectProperties, runDefinition().m_commandPublishProperties);

    if (const auto connectIntervals = commandLine().getOptionValue("connect-intervals").toInt(); connectIntervals > 0)
    {
        engine.connectIntervals(static_cast<size_t>(connectIntervals));
    }
    if (const auto resultInterval = commandLine().getOptionValue("result-interval"); !resultInterval.empty())
    {
        const auto count = String(resultInterval.substr(0, resultInterval.length() - 1)).toInt();
        const auto unit = resultInterval.back() == 'm' ? 60 : 1;
        engine.reportInterval(chrono::seconds(count * unit));
    }
    if (const auto bindInterfaces = commandLine().getOptionValue("bind-to-interfaces"); !bindInterfaces.empty())
    {
        engine.bindAddresses(Scenario::resolveBindInterfaces(bindInterfaces));
    }

    vector<RoundTripLatency> clientPublishLatencies;

    // Disconnect before the latencies the subscriber callbacks write into are destroyed: the engine's
    // own guard, at the same place and for the same reason as in xmq_scn.
    struct DisconnectGuard
    {
        ScenarioEngine& engine;

        ~DisconnectGuard()
        {
            engine.disconnectClients();
        }
    } disconnectGuard {engine};

    engine.connectClients(clientPublishLatencies, engine.name());
    if (ScenarioEngine::typeFromString(engine.type()) != ScenarioEngine::Type::Connections)
    {
        engine.publish(clientPublishLatencies, engine.name());
    }
}

void ClusterTestRunner::runTimeline()
{
    for (const auto& step : m_test.m_timeline)
    {
        waitUntil(step.m_at);
        switch (step.m_action)
        {
            case CClusterTimelineStep::Action::StopNode:
            case CClusterTimelineStep::Action::StartNode:
                stopOrStartNode(step);
                break;
            case CClusterTimelineStep::Action::Checks:
                runChecks(step.m_check);
                break;
        }
    }
}

void ClusterTestRunner::waitUntil(const chrono::seconds at) const
{
    const auto when = m_loadStarted + at;
    if (chrono::steady_clock::now() < when)
    {
        this_thread::sleep_until(when);
    }
}

void ClusterTestRunner::stopOrStartNode(const CClusterTimelineStep& step)
{
    const auto& node = m_test.node(step.m_node);
    const bool  stopping = step.m_action == CClusterTimelineStep::Action::StopNode;
    string      command = stopping ? node.m_stopCommand : node.m_startCommand;

    // {node} stands for the node's name, so a stand whose commands take a node as an argument has the
    // name written once in the test file and not once per node.
    for (auto at = command.find("{node}"); at != string::npos; at = command.find("{node}", at))
    {
        command.replace(at, 6, node.m_name);
    }

    const auto started = chrono::steady_clock::now();
    const int  status = system(command.c_str());
    const auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - started).count();

    // The exit code of the command, as a shell would report it: what system() returns is a wait
    // status, and printing that as a number says nothing to anybody.
#ifdef _WIN32
    const int exitCode = status;
#else
    const int exitCode = WEXITSTATUS(status);
#endif

    if (exitCode != 0)
    {
        // A step that failed is a test that did not happen: the checks that follow would pass against
        // a cluster nothing was done to, and say so with confidence. So it is a failure of the test,
        // and the command is printed to say which one.
        ++m_failedSteps;
        CERR(format("{}  {} {} FAILED: '{}' exited with {}",
                    stamp(m_loadStarted), node.m_name, stopping ? "stop" : "start", command, exitCode));
        return;
    }

    COUT(format("{}  {} {} ({:.1f}s)", stamp(m_loadStarted), node.m_name,
                stopping ? "stopped" : "started", static_cast<double>(elapsed) / 1000.0));
}

void ClusterTestRunner::runChecks(const string& only)
{
    for (size_t index = 0; index < m_test.m_checks.size(); ++index)
    {
        if (!only.empty() && m_test.m_checks[index].m_name != only)
        {
            continue;
        }
        if (m_ran[index])
        {
            continue;
        }
        m_ran[index] = true;

        const auto& check = m_test.m_checks[index];
        string      detail;
        const bool  passed = runCheck(check, detail);

        COUT(format("{}  {}  {}: {}", stamp(m_loadStarted), passed ? "PASS" : "FAIL", check.m_name, detail));
        m_outcomes.push_back({check.m_name, passed, detail});
    }
}

bool ClusterTestRunner::runCheck(const CClusterCheck& check, string& detail)
{
    try
    {
        switch (check.m_kind)
        {
            case CClusterCheck::Kind::Redis:
                return checkRedis(check, detail);
            case CClusterCheck::Kind::MqttConnect:
                return checkConnect(check, detail);
            case CClusterCheck::Kind::MqttDeliver:
                return checkDeliver(check, detail);
            case CClusterCheck::Kind::MqttRetained:
                return checkRetained(check, detail);
        }
    }
    catch (const exception& e)
    {
        detail = format("the check itself failed: {}", e.what());
        return false;
    }
    return false;
}

bool ClusterTestRunner::checkRedis(const CClusterCheck& check, string& detail)
{
    RedisConnect redis;
    redis.connect(URL(check.m_uri));

    // A Redis command is the words it goes to the server as, and the file gives it the way it would be
    // typed: "ZCARD cluster:members".
    const auto arguments = words(check.m_command);
    RedisCommand command(arguments.front());
    if (arguments.size() > 1)
    {
        command.emplace_back(vector<string>(arguments.begin() + 1, arguments.end()));
    }

    vector<Variant> results;
    redis.executeCommand(command, results);
    redis.disconnect();

    string answer;
    for (const auto& result : results)
    {
        if (!answer.empty())
        {
            answer += ", ";
        }
        answer += string(result.asString().c_str());
    }
    answer = trimmed(answer);

    const bool passed = !check.m_expect.empty()     ? answer == check.m_expect
                        : !check.m_contains.empty() ? answer.find(check.m_contains) != string::npos
                        : check.m_nonEmpty          ? !answer.empty()
                        : check.m_empty             ? answer.empty()
                                                    : !answer.empty() && stoll(answer) >= check.m_min;

    detail = passed ? format("{} = '{}'", check.m_command, answer)
                    : format("{} = '{}', expected {}", check.m_command, answer, expectation(check));
    return passed;
}

bool ClusterTestRunner::checkConnect(const CClusterCheck& check, string& detail)
{
    const auto address = addressFor(check.m_node, check.m_host, check.m_port);
    const Host server(address.m_host, address.m_port);

    client::MqttClient client(nullptr, "", "");
    bool               connected = false;
    string             error;

    try
    {
        const auto reasonCode = client.connect(server, credentials(checkClientId(check.m_name)),
                                               runDefinition().m_connectParameters,
                                               checkProtocolVersion(), {}, runDefinition().m_sslKeys);
        connected = reasonCode == ReasonCode::Success;
        if (!connected)
        {
            error = toString(reasonCode);
        }
    }
    catch (const exception& e)
    {
        error = e.what();
    }

    if (connected)
    {
        client.disconnect();
    }

    if (connected == check.m_expectConnected)
    {
        detail = connected ? format("{} connected", server.toString())
                           : format("{} refused: {}", server.toString(), error);
        return true;
    }

    detail = connected ? format("{} connected, and the check expects it not to", server.toString())
                       : format("{} refused: {}", server.toString(), error);
    return false;
}

bool ClusterTestRunner::checkDeliver(const CClusterCheck& check, string& detail)
{
    return carryMessage(check, false, detail);
}

bool ClusterTestRunner::checkRetained(const CClusterCheck& check, string& detail)
{
    return carryMessage(check, true, detail);
}

bool ClusterTestRunner::carryMessage(const CClusterCheck& check, const bool retain, string& detail)
{
    const auto from = addressFor(check.m_publishNode, check.m_publishHost, check.m_publishPort);
    const auto to = addressFor(check.m_subscribeNode, check.m_subscribeHost, check.m_subscribePort);
    const Host publishServer(from.m_host, from.m_port);
    const Host subscribeServer(to.m_host, to.m_port);
    // Ids of this check's own, so that carrying a message nowhere near a session somebody else
    // holds; see checkClientId().
    const auto publisherId = checkClientId(check.m_name + "-publisher");
    const auto subscriberId = checkClientId(check.m_name + "-subscriber");
    const auto parameters = runDefinition().m_connectParameters;
    const auto version = checkProtocolVersion();
    const auto& sslKeys = runDefinition().m_sslKeys;

    // The message is taken under the lock the waiting side holds, so one that arrives between the
    // publish and the wait is not lost - which across a cluster, where the two ends are different
    // nodes, is a matter of milliseconds either way.
    mutex              receivedMutex;
    condition_variable receivedCondition;
    string             receivedPayload;
    bool               received = false;

    client::MqttClient publisher(nullptr, "", "");
    client::MqttClient subscriber(nullptr, "", "");

    subscriber.onMessage(
        [&](const SPublishMessage& message)
        {
            {
                const scoped_lock lock(receivedMutex);
                if (received)
                {
                    return;
                }
                received = true;
                receivedPayload.assign(message->payload());
            }
            receivedCondition.notify_all();
        });

    // A retained message is what a client reads when it connects long after the publisher is gone, and
    // a node that was away is the case it exists for: it is published before anyone subscribes.
    if (retain)
    {
        if (const auto reasonCode = publisher.connect(publishServer, credentials(publisherId),
                                                      parameters, version, {}, sslKeys);
            reasonCode != ReasonCode::Success)
        {
            detail = format("could not publish the retained message on {}: {}",
                            publishServer.toString(), toString(reasonCode));
            return false;
        }
        publisher.publish(check.m_topic, check.m_payload, Qos::Qos1, true);
        publisher.flush();
        publisher.disconnect();

        // The retained set reaches the other nodes as a cluster message of its own, so a read that
        // follows the publish at once races with it. A test that needs to know the set has arrived
        // looks in the storage instead; this is only to keep the two apart for a moment.
        this_thread::sleep_for(chrono::seconds(1));
    }

    if (const auto reasonCode = subscriber.connect(subscribeServer, credentials(subscriberId),
                                                   parameters, version, {}, sslKeys);
        reasonCode != ReasonCode::Success)
    {
        detail = format("could not subscribe on {}: {}", subscribeServer.toString(), toString(reasonCode));
        return false;
    }
    subscriber.subscribe(check.m_topic);

    // The node the subscriber is on knows the subscription at once; the node the publication will
    // be sent to learns of it when the cluster tells it, and a check that publishes before that has
    // happened is asking about the timing rather than about the route.
    if (check.m_settle.count() > 0)
    {
        this_thread::sleep_for(check.m_settle);
    }

    // What the node itself says, before anything is published: whether the subscription is there at
    // all. A delivery that failed cannot tell these apart, which is the reason this exists.
    if (!check.m_verifyCommand.empty())
    {
        auto answer = captureCommand(check.m_verifyCommand);
        if (answer.find(check.m_verifyContains) == string::npos)
        {
            if (answer.length() > 300)
            {
                answer.resize(300);
            }
            detail = format("the node does not hold the subscription: '{}' answered '{}'",
                            check.m_verifyCommand, answer);
            subscriber.disconnect();
            return false;
        }
    }

    if (!retain)
    {
        if (const auto reasonCode = publisher.connect(publishServer, credentials(publisherId),
                                                      parameters, version, {}, sslKeys);
            reasonCode != ReasonCode::Success)
        {
            detail = format("could not publish on {}: {}", publishServer.toString(), toString(reasonCode));
            subscriber.disconnect();
            return false;
        }
        publisher.publish(check.m_topic, check.m_payload, Qos::Qos1);
        publisher.flush();
        publisher.disconnect();
    }

    {
        unique_lock<mutex> lock(receivedMutex);
        receivedCondition.wait_for(lock, check.m_timeout, [&] { return received; });
    }
    subscriber.disconnect();

    if (retain)
    {
        // Cleared afterwards, so that a second run of the test does not find the topic already set and
        // pass for a reason that has nothing to do with this run.
        client::MqttClient cleaner(nullptr, "", "");
        if (const auto reasonCode = cleaner.connect(publishServer, credentials(publisherId),
                                                    parameters, version, {}, sslKeys);
            reasonCode == ReasonCode::Success)
        {
            cleaner.publish(check.m_topic, string(), Qos::Qos1, true);
            cleaner.flush();
            cleaner.disconnect();
        }
    }

    if (!received)
    {
        detail = format("nothing arrived on {} within {}s", subscribeServer.toString(), check.m_timeout.count());
        return false;
    }
    if (receivedPayload != check.m_payload)
    {
        detail = format("'{}' arrived on {}, expected '{}'", receivedPayload, subscribeServer.toString(),
                        check.m_payload);
        return false;
    }
    detail = format("'{}' arrived on {}", receivedPayload, subscribeServer.toString());
    return true;
}

int ClusterTestRunner::report() const
{
    size_t passed = 0;
    size_t failed = 0;

    COUT("");
    COUT("Checks:");
    for (const auto& outcome : m_outcomes)
    {
        outcome.m_passed ? ++passed : ++failed;
        COUT(format("  {}  {}: {}", outcome.m_passed ? "PASS" : "FAIL", outcome.m_name, outcome.m_detail));
    }

    const auto notRun = m_ran.size() - m_outcomes.size();
    if (notRun > 0)
    {
        COUT(format("  ----  {} check(s) did not run", notRun));
    }

    if (m_failedSteps > 0)
    {
        CERR(format("  ----  {} timeline step(s) failed: the cluster was not changed the way the test "
                    "says, so the checks above passed against a cluster nothing was done to",
                    m_failedSteps));
    }

    COUT("");
    COUT(format("Cluster test '{}': {} passed, {} failed.", m_test.m_name, passed, failed));

    return failed == 0 && notRun == 0 && m_failedSteps == 0 ? 0 : 1;
}

ClusterTestRunner::CAddress ClusterTestRunner::addressFor(const string& nodeName, const string& host,
                                                          const uint16_t port) const
{
    if (!nodeName.empty())
    {
        const auto& node = m_test.node(nodeName);
        if (node.m_host.empty() || node.m_port == 0)
        {
            throw Exception(format("Node '{}' has no 'host' and 'port', so a check cannot reach it.",
                                   nodeName));
        }
        return {node.m_host, node.m_port};
    }

    if (!host.empty())
    {
        return {host, port};
    }

    return {m_test.m_serverHost, m_test.m_serverPort};
}

ConnectCredentials ClusterTestRunner::credentials(const string_view clientId) const
{
    return {string(clientId), m_test.m_username, m_test.m_password};
}

string ClusterTestRunner::checkClientId(const string_view checkName) const
{
    // The number is what makes it fresh even when the same check runs twice, and the prefix keeps it
    // out of the way of the load's own ids.
    return format("xmq_scn_cluster-{}-{}", string(checkName).substr(0, 24),
                  ++m_checkClientSequence);
}

string ClusterTestRunner::captureCommand(const string& command) const
{
    // popen rather than a pipe of our own: the command is a shell command by design - a curl against
    // a node's web service, as likely as anything else - and the tool already runs the nodes
    // themselves through a shell.
    string output;
    FILE*  pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return output;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    pclose(pipe);
    return output;
}

ProtocolVersion ClusterTestRunner::checkProtocolVersion() const
{
    return arguments().optionSpecified("protocol-version") ? runDefinition().m_protocolVersion
                                                          : ProtocolVersion::MqttV5;
}

} // namespace xmq
