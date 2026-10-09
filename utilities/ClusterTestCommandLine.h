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

#include "UtilityCommandLine.h"

#include <set>

namespace xmq {

class ClusterTestCommandLine final : public UtilityCommandLine
{
public:
    /**
     * @brief Constructor.
     * @param args Command line arguments.
     */
    explicit ClusterTestCommandLine(const std::vector<std::string>& args);

    /**
     * @brief Destructor.
     */
    ~ClusterTestCommandLine() override = default;

    /**
     * @brief Check if an option was explicitly provided on the command line.
     * Unlike hasOption(), returns false for options that only carry their default value: only the
     * ones actually given may override what the test file says.
     * @param name Full option name.
     */
    bool optionSpecified(const sptk::String& name) const
    {
        return m_specifiedOptions.contains(name);
    }

private:
    std::set<sptk::String, std::less<>> m_specifiedOptions;
};

} // namespace xmq
