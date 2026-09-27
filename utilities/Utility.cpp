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

#include "Utility.h"
#include "ExecuteParallel.h"
#include "base/xmq-config.h"
#include "common/PaceMaker.h"

#include <sptk5/StreamLogEngine.h>

using namespace std;
using namespace sptk;
using namespace xmq;

Utility::Utility(std::shared_ptr<UtilityCommandLine> commandLine)
    : m_commandLine(std::move(commandLine))
    , m_topicManager(make_shared<TopicManager>())
{
    // Answered before help and before anything that can fail, so it stays usable for
    // recording the client version alongside benchmark results.
    if (m_commandLine->hasOption("version"))
    {
        COUT(XMQ_VERSION_NUMBER);
        exit(0);
    }

    if (!m_commandLine->getError().empty() || m_commandLine->hasOption("help"))
    {
        constexpr int defaultScreenWidth = 120;
        m_commandLine->printHelp(defaultScreenWidth);
        exit(0);
    }
    m_runDefinition.load(*m_commandLine);

    // The screen, and only the screen. These are client-side tools whose every message was printed
    // to stdout anyway, so the log file they used to write beside themselves duplicated it exactly -
    // while creating a file in whatever directory the run started in, which nobody had asked for.
    // Two of them run there at once then fought over the same file, and a file they could not write
    // ended the run. Anyone who wants a durable record can pipe the output through tee.
    m_logEngine = make_shared<StreamLogEngine>(cout);
    setupLogEngine(m_runDefinition.m_logPriority);
}

void Utility::setupLogEngine(const LogPriority logPriority) const
{
    m_logEngine->option(LogEngine::Option::DATE, true);
    m_logEngine->option(LogEngine::Option::TIME, true);
    m_logEngine->option(LogEngine::Option::MILLISECONDS, true);
    // Not STDOUT: that option lives in LogEngine and duplicates every message to stdout, which is
    // where this engine already writes - each line would appear twice.
    m_logEngine->minPriority(logPriority);
}

const UtilityCommandLine& Utility::commandLine() const
{
    return *m_commandLine;
}

const RunDefinition& Utility::runDefinition() const
{
    return m_runDefinition;
}

shared_ptr<LogEngine> Utility::logEngine() const
{
    return m_logEngine;
}

void Utility::createClients()
{
    m_clients.clear();
    size_t i = 0;
    if (m_runDefinition.m_bindAddresses.empty())
    {
        m_runDefinition.m_bindAddresses.push_back("");
    }

    while (i < m_runDefinition.m_sessionCount)
    {
        for (const auto& bindAddress: m_runDefinition.m_bindAddresses)
        {
            auto client = make_shared<UtilityClient>(m_logEngine, bindAddress);
            m_clients.push_back(client);
            ++i;
            client->setClientIndex(i);
            if (i >= m_runDefinition.m_sessionCount)
            {
                break;
            }
        }
    }
}

namespace {
string nextClientId(const std::string& baseName)
{
    static mutex  idMutex;
    static size_t id = 0;

    lock_guard lock(idMutex);
    ++id;
    return baseName + to_string(id);
}
} // namespace

size_t Utility::connectClients() const
{
    SynchronizedQueue<SUtilityClient> queue;
    for (const auto& client: m_clients)
    {
        queue.push_back(client);
    }

    atomic_size_t        connectedCounter = 0;
    vector<future<void>> futures;
    string               baseClientName = string(m_runDefinition.m_credentials->getClientId()) + "-";
    auto                 paceMaker = m_runDefinition.m_connectRate > 0 ? make_shared<PaceMaker>(m_runDefinition.m_connectRate) : nullptr;

    Reporter reporter("Connect " + to_string(m_clients.size()) + " clients", {"total clients", "ms/batch", "clients K/sec"},
                      runDefinition().m_showCounters);

    ExecuteParallel parallel(
        reporter,
        clients(),
        [this, baseClientName, &connectedCounter, &paceMaker](const SUtilityClient& client)
        {
            auto               clientId = m_runDefinition.m_sessionCount > 1 ? nextClientId(baseClientName) : m_runDefinition.m_credentials->getClientId();
            ConnectCredentials credentials(
                clientId,
                m_runDefinition.m_credentials->getUsername(),
                m_runDefinition.m_credentials->getPassword());

            client->onAck(
                [&connectedCounter](const SMessage& message)
                {
                    if (message->is(Message::Type::ConnectAck))
                    {
                        ++connectedCounter;
                    }
                });

            if (paceMaker)
            {
                paceMaker->next();
            }

            const auto result = client->connect(*m_runDefinition.m_serverHost, credentials,
                                                m_runDefinition.m_connectParameters,
                                                m_runDefinition.m_protocolVersion,
                                                m_runDefinition.m_connectProperties,
                                                m_runDefinition.m_sslKeys);

            return result == ReasonCode::Success;
        });

    parallel.execute(runDefinition().m_showCounters, 64);

    return connectedCounter;
}

void Utility::disconnectClients() const
{
    for (const auto& client: m_clients)
    {
        client->disconnect();
    }
}

const std::vector<SUtilityClient>& Utility::clients() const
{
    return m_clients;
}

String Utility::substituteVariables(const String& value, const SUtilityClient& client)
{
    return value.replace("%ClientIndex%", to_string(client->getClientIndex()));
}

String Utility::printSummary(const std::string_view operation, const size_t objectCount, const std::string_view objectsName, const double operationSeconds)
{
    stringstream   summary;
    constexpr auto decimalPoints = 2;
    summary << operation << " " << objectCount << " " << objectsName
            << " for " << fixed << setprecision(decimalPoints);
    if (operationSeconds < 1)
    {
        summary << operationSeconds * 1000 << "ms, ";
    }
    else
    {
        summary << operationSeconds << "s, ";
    }

    if (const auto rate = static_cast<double>(objectCount) / operationSeconds;
        rate > 2000)
    {
        constexpr auto thousand = 1000;
        summary << fixed << setprecision(2) << rate / thousand << "K/s";
    }
    else
    {
        summary << static_cast<int>(rate) << "/s";
    }

    return summary.str();
}
