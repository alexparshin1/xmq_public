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

#include "base/MessageProperties.h"

#include <map>
#include <sptk5/Strings.h>

namespace xmq {

/**
 * @brief The MQTT5 properties a command line asked for, kept apart by the packet they belong to.
 *
 * The utilities take them as "-D <command> <name> <value>", where the command is "connect" or
 * "publish": a CONNECT's Receive Maximum and a PUBLISH's User Property are different things sent at
 * different moments, and one option that meant both would be useless for measuring either.
 *
 * The option has existed for a long time and did nothing at all: the three words were read from the
 * argument queue without being taken off it, so they were collected as one word repeated and then
 * rejected as unexpected arguments - and nothing anywhere turned the result into properties. This
 * is that missing half.
 */
class CommandProperties
{
public:
    /**
     * @brief Parse the definitions the command line collected.
     * @param definitions   Strings shaped "command:name=value", as UtilityCommandLine assembles them.
     * @throws sptk::Exception when a name is not a property, or its value does not fit its type.
     */
    explicit CommandProperties(const sptk::Strings& definitions);

    /**
     * @brief The properties for one packet, or nullptr when none were asked for.
     *
     * Nullptr and not an empty set, because an empty MQTT5 property block is a byte on the wire and
     * a measurement of nothing: a run that asked for no properties should send none.
     *
     * @param command       "connect" or "publish".
     */
    [[nodiscard]] std::shared_ptr<MessageProperties> forCommand(const sptk::String& command) const;

private:
    std::map<sptk::String, std::shared_ptr<MessageProperties>> m_byCommand;
};

} // namespace xmq
