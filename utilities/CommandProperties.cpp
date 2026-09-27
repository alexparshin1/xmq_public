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

#include "utilities/CommandProperties.h"

#include "base/MessageProperties.h"

using namespace std;
using namespace sptk;
using namespace xmq;

CommandProperties::CommandProperties(const Strings& definitions)
{
    for (const auto& definition: definitions)
    {
        // "command:name=value" - the shape UtilityCommandLine assembles from "-D command name value".
        const auto colon = definition.find(':');
        const auto equals = definition.find('=', colon == String::npos ? 0 : colon);
        if (colon == String::npos || equals == String::npos)
        {
            throw Exception("Cannot read property definition \"" + definition + "\"");
        }

        const auto command = String(definition.substr(0, colon)).toLowerCase();
        const auto name = definition.substr(colon + 1, equals - colon - 1);
        const auto value = definition.substr(equals + 1);

        if (command != "connect" && command != "publish")
        {
            throw Exception("Property \"" + name + "\" is for " + command +
                            ", and only connect and publish carry properties");
        }

        auto& properties = m_byCommand[command];
        if (!properties)
        {
            properties = make_shared<MessageProperties>();
        }
        // The same conversion the scenario file uses. It lived here in a second copy until
        // scenarios needed it too, and two copies of a type table drift.
        setPropertyFromText(*properties, name, value);
    }
}

shared_ptr<MessageProperties> CommandProperties::forCommand(const String& command) const
{
    const auto found = m_byCommand.find(String(command).toLowerCase());
    return found == m_byCommand.end() ? shared_ptr<MessageProperties> {} : found->second;
}
