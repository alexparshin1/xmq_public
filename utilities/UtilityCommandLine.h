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

#include <sptk5/CommandLine.h>

namespace xmq {

class UtilityCommandLine : public sptk::CommandLine
{
public:
    UtilityCommandLine(const sptk::String& programVersion, const sptk::String& description, const sptk::String& commandLinePrototype);
    virtual ~UtilityCommandLine() = default;
    static std::vector<const char*> PreprocessCommandLine(const std::vector<std::string>& args);

    /**
     * @brief Server URI parser regex.
     * @return Server URI parser regex.
     */
    static const sptk::RegularExpression& uriParser();

    /**
     * @return Command line error.
     */
    [[nodiscard]] sptk::String getError() const
    {
        return m_error;
    }

protected:
    void initCommandLine(const std::vector<std::string>& args);

private:
    static std::vector<std::string> m_processedArguments;
    static sptk::RegularExpression  m_parseURI;
    sptk::String                    m_error;
};

} // namespace xmq
