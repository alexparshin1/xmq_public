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

#include "ListenerManager.h"
#include "Settings.h"

using namespace std;
using namespace sptk;
using namespace xmq;

ListenerManager::ListenerManager(Settings* settings, const OnChangeEvent& onChange)
    : m_settings(settings)
    , m_onChange(onChange)
{
}

void ListenerManager::initialize(bool& changed)
{
    setMissingIds(m_settings->m_connections.m_listener, changed);
}

uint64_t ListenerManager::control(const string& action, const CListener& listener)
{
    auto&      listeners = m_settings->m_connections.m_listener;
    auto       id = listener.m_id.asInt64();
    const auto port = listener.m_port.asInteger();
    auto       foundId = WSArray<CListener>::iterator {listeners.end()};
    auto       foundPort = WSArray<CListener>::iterator {listeners.end()};
    for (auto iterator = listeners.begin(); iterator != listeners.end(); ++iterator)
    {
        if (iterator->m_id.asInt64() == id)
        {
            foundId = iterator;
        }
        if (iterator->m_port.asInteger() == port)
        {
            foundPort = iterator;
        }
    }

    if (action == "add")
    {
        CListener newListener(listener);
        newListener.m_id.setInt64(nextSerialId());
        if (foundId != listeners.end() || foundPort != listeners.end())
        {
            throw invalid_argument("Listener already exists, or port is used: " + listener.m_name.asString());
        }
        listeners.push_back(newListener);
        id = newListener.m_id.asInt64();
    }
    else if (action == "modify")
    {
        if (foundId == listeners.end())
        {
            throw invalid_argument("Listener doesn't exist: " + listener.m_name.asString());
        }

        if (foundPort != listeners.end() && foundId != foundPort)
        {
            throw invalid_argument("Port is used by another listener: " + foundPort->m_name.asString());
        }
        *foundId = listener;
    }
    else if (action == "remove")
    {
        if (foundId == listeners.end())
        {
            throw invalid_argument("Listener doesn't exist: id=" + to_string(id));
        }
        listeners.erase(foundId);
    }
    else
    {
        throw invalid_argument("Invalid action: " + action);
    }

    m_onChange();

    return id;
}
