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

#include "base/DescriptorLimit.h"
#include "Server.h"
#include <format>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
String makeLogPrefix(const String& nodeName)
{
    return format("({}) ", nodeName.c_str());
}
} // namespace

ServerData::ServerData(const shared_ptr<Settings>& settings, const shared_ptr<LogEngine>& logEngine, const SocketPoolTriggerMode triggerMode)
    // The reactor's registration map holds one entry per connection and rehashes everything it
    // holds when it grows, under the lock every event needs. Sized once from the descriptor limit,
    // it never grows. See base/DescriptorLimit.h.
    : FastTCPServer("Server", logEngine, triggerMode, 32, sptk::DEFAULT_LISTEN_BACKLOG,
                    possibleConnectionCount(1000000))
    , m_logEngine(logEngine)
    , m_logger(make_shared<Logger>(*logEngine, makeLogPrefix(settings->m_cluster.m_this_node.m_node_name)))
    , m_settings(settings)
    , m_topicManager(make_shared<TopicManager>())
    , m_genericProtocols(make_shared<GenericProtocols>(m_topicManager))
    , m_clientSessionManager(make_shared<ClientSessionManager>(reinterpret_cast<Server*>(this)))
{
    const unique_lock lock(m_mutex);
    m_nodeName = m_settings->m_cluster.m_this_node.m_node_name.asString().c_str();
    m_logEngine->minPriority(m_settings->getLogPriority());
}

ServerData::~ServerData()
{
    FastTCPServer::stop();
    m_clientSessionManager.reset();
    m_genericProtocols.reset();
    m_topicManager.reset();
    m_logger.reset();
    m_logEngine = nullptr;
}

shared_ptr<LogEngine> ServerData::getLogEngine() const
{
    const shared_lock lock(m_mutex);
    return m_logEngine;
}

shared_ptr<Settings> ServerData::getSettings() const
{
    const shared_lock lock(m_mutex);
    return m_settings;
}

SClientSessionManager ServerData::getClientSessionManager() const
{
    const shared_lock lock(m_mutex);
    return m_clientSessionManager;
}

void ServerData::logMessage(const LogSubject subject, const LogPriority priority, const string& message) const
{
    const shared_lock lock(m_mutex);
    if (m_settings && m_settings->logSubjectEnabled(subject, priority))
    {
        m_logger->log(priority, format("<{}> {}", to_string(subject), message));
    }
}

void ServerData::logMessage(const LogSubject subject, const LogPriority priority, const Logger::OutputString& output) const
{
    const shared_lock lock(m_mutex);
    if (m_settings && m_settings->logSubjectEnabled(subject, priority))
    {
        m_logger->log(priority, [subject, &output]
                      {
                          return format("<{}> {}", to_string(subject), output());
                      });
    }
}

std::string ServerData::getNodeName() const
{
    const shared_lock lock(m_mutex);
    return m_nodeName;
}

RecordId ServerData::getNodeId() const
{
    const shared_lock lock(m_mutex);
    return m_nodeId;
}

void ServerData::setNodeId(const RecordId nodeId)
{
    const unique_lock lock(m_mutex);
    m_nodeId = nodeId;
}

cluster::ServerNodeState ServerData::getNodeState() const
{
    const unique_lock lock(m_mutex);
    return m_nodeState;
}

void ServerData::setNodeState(const cluster::ServerNodeState state)
{
    const unique_lock lock(m_mutex);
    m_nodeState = state;
}

void ServerData::loggerReset() const
{
    const shared_lock lock(m_mutex);
    m_logEngine->reset();
}
