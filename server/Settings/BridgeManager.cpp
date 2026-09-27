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

#include "BridgeManager.h"
#include "Settings.h"

#include <sptk5/net/Host.h>

using namespace std;
using namespace sptk;
using namespace xmq;

BridgeManager::BridgeManager(Settings* settings, const OnChangeEvent& onChange)
    : m_settings(settings)
    , m_onChange(onChange)
{
}

void BridgeManager::initialize(bool& changed)
{
    setMissingIds(m_settings->m_bridges, changed);
}

namespace {

// Endpoints are compared the way Bridge::connectAndSubscribe() resolves them, so that
// "remote" and "remote:1883" are recognised as the same broker rather than slipping past
// as two different strings. Host names are case-insensitive, node names are compared the
// same way: a pair differing only in case is a mistake, not two distinct nodes.
constexpr uint16_t DefaultMqttPort = 1883;

String normalizedEndpoint(const String& hostPort)
{
    const Host parsed(hostPort.c_str());
    const auto endpoint = parsed.port() == 0
                              ? Host(hostPort.c_str(), DefaultMqttPort).toString()
                              : parsed.toString();
    return String(endpoint).toLowerCase();
}

} // namespace

void BridgeManager::checkUnique(const CBridge& bridge, const uint64_t id) const
{
    const auto nodeName = String(bridge.m_node_name.asString()).toLowerCase();
    const auto endpoint = normalizedEndpoint(bridge.m_host_port.asString());

    for (const auto& existing: m_settings->m_bridges)
    {
        // The bridge being modified is not a duplicate of itself.
        if (existing.m_id.asInt64() == static_cast<int64_t>(id))
        {
            continue;
        }

        if (String(existing.m_node_name.asString()).toLowerCase() == nodeName)
        {
            throw Exception("Bridge name is already used: " + bridge.m_node_name.asString());
        }

        if (normalizedEndpoint(existing.m_host_port.asString()) == endpoint)
        {
            throw Exception("Bridge to " + bridge.m_host_port.asString() +
                                   " already exists: " + existing.m_node_name.asString());
        }
    }
}

uint64_t BridgeManager::control(const string& action, const CBridge& bridge)
{
    auto& bridges = m_settings->m_bridges;
    auto  id = bridge.m_id.asInt64();
    if (id == 0)
    {
        id = static_cast<int64_t>(nextSerialId());
    }
    auto foundId = WSArray<CBridge>::iterator {bridges.end()};
    for (auto iterator = bridges.begin(); iterator != bridges.end(); ++iterator)
    {
        if (iterator->m_id.asInt64() == id)
        {
            foundId = iterator;
        }
    }

    if (action == "add")
    {
        if (foundId != bridges.end())
        {
            throw Exception("Bridge already exists: " + bridge.m_node_name.asString());
        }
        checkUnique(bridge, id);
        CBridge newBridge(bridge);
        newBridge.m_id = id;
        bridges.push_back(newBridge);
        id = newBridge.m_id.asInt64();
    }
    else if (action == "modify")
    {
        if (foundId == bridges.end())
        {
            throw Exception("Bridge doesn't exist: " + bridge.m_node_name.asString());
        }
        checkUnique(bridge, id);

        if (bridge.m_password.asString() == "*****")
        {
            const auto savePassword = foundId->m_password.asString();
            *foundId = bridge;
            foundId->m_password = savePassword;
        }
        else
        {
            *foundId = bridge;
        }
    }
    else if (action == "remove")
    {
        if (foundId == bridges.end())
        {
            throw Exception("Bridge doesn't exist: id=" + to_string(id));
        }
        bridges.erase(foundId);
    }
    else
    {
        throw Exception("Invalid action: " + action);
    }

    m_onChange();

    return id;
}
