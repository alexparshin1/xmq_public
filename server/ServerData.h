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

#include "ClientSession/ClientSession.h"
#include "ClientSession/ClientSessionManager.h"
#include "Settings/Settings.h"

#include <sptk5/net/FastTCPServer.h>

namespace xmq {

class XMQ_EXPORT ServerData : public sptk::FastTCPServer
{
public:
    /**
     * @brief Constructor.
     * @param settings          Server settings.
     * @param logEngine         Log engine.
     * @param triggerMode       Socket pool trigger mode.
     */
    ServerData(const std::shared_ptr<Settings>& settings, const std::shared_ptr<sptk::LogEngine>& logEngine, sptk::SocketPoolTriggerMode triggerMode);

    /**
     * @brief Destructor.
     */
    ~ServerData() override;

    /**
     * @brief Get log engine.
     */
    std::shared_ptr<sptk::LogEngine> getLogEngine() const;

    /**
     * @brief Get server settings.
     */
    std::shared_ptr<Settings> getSettings() const;

    /**
     * @brief Find a client session.
     * @param clientId         Client ID.
     */
    SClientSession getClientSession(const sptk::String& clientId) const
    {
        return m_clientSessionManager->find(clientId);
    }

    /**
     * @brief Remove client session.
     * @param clientSession    Client session.
     */
    void removeClientSession(const SClientSession& clientSession)
    {
        clientSession->clearProtocol();
        clientSession->clearSession();
        m_clientSessionManager->remove(clientSession);
    }

    /**
     * @brief Get the client session manager.
     */
    [[nodiscard]] SClientSessionManager getClientSessionManager() const; ///< Client session manager

    /**
     * @brief Restart logger.
     */
    void loggerReset() const;

    std::shared_ptr<TopicManager> getTopicManager() const
    {
        return m_topicManager;
    }

    std::shared_ptr<GenericProtocols> getGenericProtocols() const
    {
        return m_genericProtocols;
    }

    const Topic* getTopic(const std::string_view topic) const
    {
        return m_topicManager->getTopic(topic);
    }

    bool loggerHas(const sptk::LogPriority logPriority) const
    {
        return m_logger->has(logPriority);
    }

    void logMessage(LogSubject subject, sptk::LogPriority priority, const std::string& message) const;
    void logMessage(LogSubject subject, sptk::LogPriority priority, const sptk::Logger::OutputString& output) const;

    [[nodiscard]] std::string getNodeName() const;

    [[nodiscard]] RecordId getNodeId() const;
    void                   setNodeId(RecordId nodeId);

    [[nodiscard]] cluster::ServerNodeState getNodeState() const;
    void                                   setNodeState(cluster::ServerNodeState state);

private:
    mutable std::shared_mutex             m_mutex;
    std::shared_ptr<sptk::LogEngine>      m_logEngine;                                     ///< External log engine.
    std::shared_ptr<sptk::Logger>         m_logger;                                        ///< Logger.
    std::shared_ptr<Settings>             m_settings;                                      ///< Server getSettings.
    STopicManager                         m_topicManager;                                  ///< Topic manager.
    SGenericProtocols                     m_genericProtocols;                              ///< Generic protocols.
    std::shared_ptr<ClientSessionManager> m_clientSessionManager;                          ///< Client session manager.
    std::string                           m_nodeName;                                      ///< Node name.
    RecordId                              m_nodeId {0};                                    ///< Node ID.
    cluster::ServerNodeState              m_nodeState {cluster::ServerNodeState::Offline}; ///< Node state.
};

} // namespace xmq
