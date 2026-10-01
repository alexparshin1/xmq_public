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

#include "../Cluster/ClusterTopics.h"

#include "ClientSession.h"
#include "service/CConnectionInfo.h"
#include <map>
#include <shared_mutex>

namespace xmq {

class Server;

/**
 * @brief Client session manager.
 */
class XMQ_EXPORT ClientSessionManager final
{
public:
    /**
     * @brief Constructor
     * @param server            XMQ server.
     */
    explicit ClientSessionManager(Server* server);

    /**
     * @brief Load client sessions from persistent storage.
     * @param storage           Persistent storage.
     * @param nodeId            Node id.
     */
    void load(const SStorage& storage, const std::string& nodeId);

    /**
     * @brief Add client clientSession.
     * @param clientSession           Client clientSession.
     */
    void add(const SClientSession& clientSession);

    /**
     * @brief Find a client session by client id.
     * @param clientId          Client id.
     * @return Client session, or null session if not found. Returned by value so the
     *         session stays alive even if it is removed from the manager by another thread.
     */
    [[nodiscard]] SClientSession find(const std::string& clientId);

    /**
     * @brief Clear client sessions.
     */
    void clear();

    /**
     * @brief Client session count.
     * @return.
     */
    [[nodiscard]] size_t clientCount() const;

    /**
     * @brief Report client sessions.
     * @param matchClientName   Regular expression to match client ids.
     * @return Collection of connection information.
     */
    [[nodiscard]] std::vector<CConnectionInfo> getClientConnectionsInfo(const sptk::RegularExpression& matchClientName);

    /**
     * @brief Find sessions whose client ids start with a prefix.
     * @param prefix            Client id prefix.
     * @param limit             Maximum number of matches before the extra page indicator.
     * @return At most limit + 1 sessions in client id order.
     */
    [[nodiscard]] std::vector<SClientSession> findSessions(std::string_view prefix, size_t limit) const;

    /**
     * @brief Remove the client session.
     * @param clientSession     Client session to remove.
     */
    void remove(const SClientSession& clientSession);

    /**
     * @brief Remove the client session.
     * @param clientSession     Client session to remove.
     */
    void remove(const ClientSession* clientSession);

    /**
     * @brief Execute action for each client.
     */
    void forEach(const std::function<void(const SClientSession&)>& action);

    [[nodiscard]] Server* getServer() const
    {
        return m_xmqServer;
    }

private:
    mutable std::shared_mutex                          m_mutex;                      ///< Mutex to protect client session collections.
    Server*                                            m_xmqServer;                  ///< XMQ server.
    std::map<std::string, SClientSession, std::less<>>  m_clientConnectionsByName;    ///< Ordered client ID index for prefix queries.
    XMQ_MAP_TYPE<const ClientSession*, SClientSession> m_clientConnectionsByAddress; ///< Client address to the client session map.

    /**
     * @brief Remove client session.
     * @param clientSession     Client session to remove.
     */
    /// Erases the name only while it still points at this session. See the definition for why.
    void eraseIfStillOurs(const std::string& clientId, const ClientSession* clientSession);

    void removeUnlocked(const SClientSession& clientSession);

    /**
     * @brief Remove client session.
     * @param clientId          Client ID to remove.
     */
    void removeUnlocked(const std::string& clientId);
};

using SClientSessionManager = std::shared_ptr<ClientSessionManager>;

} // namespace xmq
