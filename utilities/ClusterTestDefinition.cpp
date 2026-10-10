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

#include "ClusterTestDefinition.h"

#include <sptk5/cutils>
#include <sptk5/xdoc/Document.h>

#include <algorithm>

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

using xdoc::SNode;

/**
 * @brief The child of that name, and the immediate one: a recursive search would find a "name"
 * somewhere else entirely, in a check of a timeline step rather than in the node being read.
 */
SNode child(const SNode& parent, const string& name)
{
    if (!parent)
    {
        return {};
    }
    return parent->findFirst(String(name.c_str()), xdoc::SearchMode::ImmediateChild);
}

string text(const SNode& parent, const string& name, const string& fallback = {})
{
    const auto found = child(parent, name);
    if (!found)
    {
        return fallback;
    }
    return string(found->getText().c_str());
}

int64_t integer(const SNode& parent, const string& name, const int64_t fallback)
{
    const auto found = child(parent, name);
    if (!found)
    {
        return fallback;
    }
    return found->getInteger();
}

bool boolean(const SNode& parent, const string& name, const bool fallback)
{
    const auto found = child(parent, name);
    if (!found)
    {
        return fallback;
    }
    return found->getBoolean();
}

/** @brief A port from the file, refused rather than truncated when it is not one. */
uint16_t port(const SNode& parent, const string& name, const uint16_t fallback)
{
    const auto value = integer(parent, name, fallback);
    if (value <= 0 || value > 65535)
    {
        throw Exception(format("'{}' is not a port number: {}.", name, value));
    }
    return static_cast<uint16_t>(value);
}

} // namespace

void ClusterTestDefinition::load(const std::filesystem::path& testFile)
{
    Buffer buffer;
    buffer.loadFromFile(testFile);

    xdoc::Document document;
    document.load(buffer);

    const auto root = document.root();
    if (!root)
    {
        throw Exception(format("The cluster test file is empty: {}.", testFile.string()));
    }

    m_name = text(root, "name", testFile.stem().string());

    if (const auto server = child(root, "server"))
    {
        m_serverHost = text(server, "host", m_serverHost);
        m_serverPort = port(server, "port", m_serverPort);
        m_username = text(server, "username", m_username);
        m_password = text(server, "password", m_password);
    }

    // The load is the scenario an ordinary xmq_scn run would take, named rather than described here:
    // it is measured and reviewed on its own, and a test that carried its own copy would drift from
    // the one the release measures.
    const auto load = child(root, "load");
    if (!load)
    {
        throw Exception("The cluster test has no 'load' section: there is nothing to run against the cluster.");
    }
    const auto scenario = text(load, "scenario");
    if (scenario.empty())
    {
        throw Exception("'load.scenario' is empty: name the scenario file the load runs.");
    }
    m_loadScenario = scenario;
    if (m_loadScenario.is_relative())
    {
        m_loadScenario = testFile.parent_path() / m_loadScenario;
    }

    // Which node each group of the load runs against. The scenario says where its clients go, but
    // that is a fact about the cluster, and the cluster is this file's business: naming the node here
    // keeps the ports in one place and lets one scenario be run against another stand.
    for (const auto& group : load->nodes("groups"))
    {
        const auto groupName = text(group, "name");
        const auto nodeName = text(group, "node");
        if (groupName.empty() || nodeName.empty())
        {
            throw Exception("A group in 'load.groups' needs both 'name' (publishers or subscribers) "
                            "and 'node'.");
        }
        if (groupName != "publishers" && groupName != "subscribers")
        {
            throw Exception(format("The load has no group '{}': it has publishers and subscribers.",
                                   groupName));
        }
        m_loadGroups[groupName] = nodeName;
    }

    for (const auto& node : root->nodes("nodes"))
    {
        CClusterNode item;
        item.m_name = text(node, "name");
        if (item.m_name.empty())
        {
            throw Exception("A node in 'nodes' has no 'name'.");
        }
        item.m_host = text(node, "host");
        item.m_port = child(node, "port") ? port(node, "port", 0) : 0;
        item.m_stopCommand = text(node, "stop");
        item.m_startCommand = text(node, "start");
        if (item.m_stopCommand.empty() || item.m_startCommand.empty())
        {
            throw Exception(format("Node '{}' names only one of 'stop' and 'start'; both are needed, "
                                   "since a test that takes a node away has to bring it back.",
                                   item.m_name));
        }
        m_nodes.push_back(std::move(item));
    }

    for (const auto& step : root->nodes("timeline"))
    {
        CClusterTimelineStep item;

        const auto action = text(step, "action", "check");
        if (action == "stop")
        {
            item.m_action = CClusterTimelineStep::Action::StopNode;
        }
        else if (action == "start")
        {
            item.m_action = CClusterTimelineStep::Action::StartNode;
        }
        else if (action == "check")
        {
            item.m_action = CClusterTimelineStep::Action::Checks;
        }
        else
        {
            throw Exception(format("Unknown timeline action '{}': expected stop, start or check.", action));
        }

        item.m_at = chrono::seconds(integer(step, "at", 0));
        if (item.m_at < 0s)
        {
            throw Exception(format("A timeline step is at {}s; steps are timed from the start of the load.",
                                   item.m_at.count()));
        }
        item.m_node = text(step, "node");
        item.m_check = text(step, "check");

        if (item.m_action != CClusterTimelineStep::Action::Checks && item.m_node.empty())
        {
            throw Exception(format("The '{}' step at {}s names no node.", action, item.m_at.count()));
        }
        m_timeline.push_back(std::move(item));
    }

    // In the order they happen, whatever order the file lists them in: a file is easier to read
    // grouped by what it does, and there is no reason to make the reader sort it.
    stable_sort(m_timeline.begin(), m_timeline.end(),
                [](const CClusterTimelineStep& left, const CClusterTimelineStep& right)
                {
                    return left.m_at < right.m_at;
                });

    for (const auto& raw : root->nodes("checks"))
    {
        CClusterCheck item;
        item.m_name = text(raw, "name");
        if (item.m_name.empty())
        {
            throw Exception("A check in 'checks' has no 'name'; it is how a timeline step asks for it.");
        }

        const auto kind = text(raw, "kind", "redis");
        const auto action = text(raw, "action");

        if (kind == "redis")
        {
            item.m_kind = CClusterCheck::Kind::Redis;
            item.m_uri = text(raw, "uri");
            item.m_command = text(raw, "command");
            if (item.m_uri.empty() || item.m_command.empty())
            {
                throw Exception(format("Check '{}' is a redis one and needs both 'uri' and 'command'.",
                                       item.m_name));
            }
            item.m_expect = text(raw, "expect");
            item.m_contains = text(raw, "contains");
            item.m_min = integer(raw, "min", numeric_limits<long long>::min());
            item.m_nonEmpty = boolean(raw, "nonempty", false);
            item.m_empty = boolean(raw, "empty", false);

            const auto expectations = (item.m_expect.empty() ? 0 : 1) + (item.m_contains.empty() ? 0 : 1) +
                (item.m_min == numeric_limits<long long>::min() ? 0 : 1) +
                (item.m_nonEmpty ? 1 : 0) + (item.m_empty ? 1 : 0);
            if (expectations != 1)
            {
                throw Exception(format("Check '{}' has {} expectations; it needs exactly one of 'expect', "
                                       "'contains', 'min', 'nonempty' and 'empty'.",
                                       item.m_name, expectations));
            }
        }
        else if (kind == "mqtt")
        {
            item.m_node = text(raw, "node");
            item.m_host = text(raw, "host");
            item.m_port = port(raw, "port", 1883);
            item.m_topic = text(raw, "topic");
            item.m_payload = text(raw, "payload");
            item.m_timeout = chrono::seconds(integer(raw, "timeout", 10));
            item.m_settle = chrono::seconds(integer(raw, "settle", 0));
            item.m_verifyCommand = text(raw, "verify-command");
            item.m_verifyContains = text(raw, "verify-contains");
            if (!item.m_verifyCommand.empty() && item.m_verifyContains.empty())
            {
                throw Exception(format("Check '{}' runs a verification but says nothing to look for in "
                                       "its answer: give it 'verify-contains' as well.",
                                       item.m_name));
            }

            if (action == "connect")
            {
                item.m_kind = CClusterCheck::Kind::MqttConnect;
                item.m_expectConnected = boolean(raw, "expect", true);
                if (item.m_host.empty() && item.m_node.empty())
                {
                    throw Exception(format("Check '{}' connects somewhere: give it a 'node', or a 'host'.",
                                           item.m_name));
                }
            }
            else if (action == "deliver" || action == "retained")
            {
                item.m_kind = action == "deliver" ? CClusterCheck::Kind::MqttDeliver
                                                  : CClusterCheck::Kind::MqttRetained;
                item.m_publishNode = text(raw, "publish-node");
                item.m_publishHost = text(raw, "publish-host", item.m_host);
                item.m_publishPort = port(raw, "publish-port", item.m_port);
                item.m_subscribeNode = text(raw, "subscribe-node");
                item.m_subscribeHost = text(raw, "subscribe-host", item.m_host);
                item.m_subscribePort = port(raw, "subscribe-port", item.m_port);
                if (item.m_topic.empty())
                {
                    throw Exception(format("Check '{}' carries a message: give it a 'topic'.", item.m_name));
                }
                if (item.m_publishHost.empty() && item.m_publishNode.empty())
                {
                    throw Exception(format("Check '{}' needs a node to publish on: give it 'publish-node', "
                                           "or 'publish-host'.", item.m_name));
                }
                if (item.m_subscribeHost.empty() && item.m_subscribeNode.empty())
                {
                    throw Exception(format("Check '{}' needs a node to receive on: give it 'subscribe-node', "
                                           "or 'subscribe-host'.", item.m_name));
                }
            }
            else
            {
                throw Exception(format("Unknown action '{}' of check '{}': expected connect, deliver or retained.",
                                       action, item.m_name));
            }
        }
        else
        {
            throw Exception(format("Unknown kind '{}' of check '{}': expected redis or mqtt.",
                                   kind, item.m_name));
        }

        m_checks.push_back(std::move(item));
    }

    // A step that names a node or a check the test does not have is a test that cannot mean what it
    // says, and it is cheaper to hear about it now than in the middle of a run.
    for (const auto& step : m_timeline)
    {
        if (!step.m_node.empty())
        {
            static_cast<void>(node(step.m_node));
        }
        if (!step.m_check.empty())
        {
            static_cast<void>(check(step.m_check));
        }
    }

    // The same for a check that names a node, and a node that is named has to have an address: a
    // check that cannot say where to connect is a check that will fail in the middle of a run, and
    // the message there says nothing about the test file.
    for (const auto& item : m_checks)
    {
        for (const auto& referenced : {item.m_node, item.m_publishNode, item.m_subscribeNode})
        {
            if (referenced.empty())
            {
                continue;
            }
            const auto& found = node(referenced);
            if (found.m_host.empty() || found.m_port == 0)
            {
                throw Exception(format("Check '{}' names node '{}', which has no 'host' and 'port' in "
                                       "'nodes', so there is nowhere to connect.",
                                       item.m_name, referenced));
            }
        }
    }

    for (const auto& [groupName, nodeName] : m_loadGroups)
    {
        const auto& found = node(nodeName);
        if (found.m_host.empty() || found.m_port == 0)
        {
            throw Exception(format("The load's '{}' group is pointed at node '{}', which has no 'host' "
                                   "and 'port' in 'nodes'.", groupName, nodeName));
        }
    }
}

const CClusterNode& ClusterTestDefinition::node(const string_view name) const
{
    const auto found = find_if(m_nodes.begin(), m_nodes.end(),
                               [name](const CClusterNode& item) { return item.m_name == name; });
    if (found == m_nodes.end())
    {
        throw Exception(format("The test has no node '{}'.", name));
    }
    return *found;
}

const CClusterCheck& ClusterTestDefinition::check(const string_view name) const
{
    const auto found = find_if(m_checks.begin(), m_checks.end(),
                               [name](const CClusterCheck& item) { return item.m_name == name; });
    if (found == m_checks.end())
    {
        throw Exception(format("The test has no check '{}'.", name));
    }
    return *found;
}

} // namespace xmq
