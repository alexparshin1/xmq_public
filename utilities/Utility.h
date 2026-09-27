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

#include "PublisherCommandLine.h"
#include "RunDefinition.h"
#include "UtilityClient.h"
#include "client/MqttClient.h"
#include <sptk5/cutils>

namespace xmq {

class Utility
{
public:
    /**
     * @brief Constructor.
     * @param commandLine       Command line.
     */
    explicit Utility(std::shared_ptr<UtilityCommandLine> commandLine);

    /**
     * @brief Destructor.
     */
    virtual ~Utility() = default;

    /**
     * @brief Run the application (send messages).
     * @return Exit code.
     */
    virtual int run() = 0;

    const UtilityCommandLine&            commandLine() const;
    const RunDefinition&                 runDefinition() const;
    std::shared_ptr<sptk::LogEngine>     logEngine() const;
    const std::vector<SUtilityClient>&   clients() const;

    void   createClients();
    size_t connectClients() const;
    void   disconnectClients() const;

    static sptk::String printSummary(std::string_view operation, size_t objectCount, std::string_view objectsName, double operationSeconds);

protected:
    void                setupLogEngine(sptk::LogPriority logPriority) const;
    static sptk::String substituteVariables(const sptk::String& value, const SUtilityClient& client);

private:
    /// Created in the constructor, not here: the constructor answers --help and --version before
    /// anything else, and a log engine built as a member would run ahead of that.
    std::shared_ptr<sptk::LogEngine>     m_logEngine;
    std::vector<SUtilityClient>          m_clients;
    std::shared_ptr<UtilityCommandLine>  m_commandLine;
    RunDefinition                        m_runDefinition;
    STopicManager                        m_topicManager;
};

} // namespace xmq
