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

#include "ClientSessionData.h"
#include "server/MessageDelivery.h"

#include <optional>

namespace xmq {

class ClientSession;
class Server;

class PersistentClientSession
    : public ClientSessionData
    , public PersistentObject
{
public:
    /**
     * @brief Session initialization type.
     */
    enum class SessionInitType : uint8_t
    {
        New,      ///< Created new session.
        Restored, ///< Restored existing session.
        Error     ///< Couldn't load session.
    };

    enum class SessionStoreType : uint8_t
    {
        SessionData,
        SessionDataAndNodeLink
    };

    /**
     * @brief Constructor.
     * @param server Server.
     * @param connectMessageParameters Connect message parameters.
     * @param connectMessageProperties Connect message properties.
     */
    explicit PersistentClientSession(Server*                          server,
                                     const SConnectMessageParameters& connectMessageParameters,
                                     const SMessageProperties&        connectMessageProperties);

    /**
     * @brief Store session to Redis.
     * @remarks Asynchronously store session data, if exists, and link session data to its node.
     * @param sessionStoreType
     */
    void redisStoreSession(SessionStoreType sessionStoreType);

    /**
     * @brief Remove session from Redis.
     * @remarks Asynchronously remove session data, if exists, and unlink session data from its node.
     * @param clientId Client ID.
     * @param removeSessionMessages If true, then remove session messages.
     */
    void redisRemoveSession(const std::string& clientId, bool removeSessionMessages);

    /**
     * @brief Remove session from Redis.
     * @remarks Remove session data, if exists, and unlink session data from its node.
     * @param sessionData Session data.
     * @param removeSessionMessages If true, then remove session messages.
     */
    void redisRemoveSession(const sptk::Buffer& sessionData, bool removeSessionMessages);

    /**
     * @brief Destructor.
     */
    ~PersistentClientSession() override = default;

    bool load();

    /**
     * @brief The session's record, already fetched from Redis, for the next load() to use.
     *
     * The CONNECT path looks it up asynchronously so that no thread waits on Redis for it (see
     * Server::lookUpSessionThen()), and hands the answer over here; load() then takes it instead of
     * asking again. A null Variant is an answer too: no such session.
     */
    void setPrefetchedSession(sptk::Variant record)
    {
        m_prefetchedSession = std::move(record);
    }

    void persist();
    void unpersist();

    bool initSession(const std::string& clientId, bool cleanSession);

    /**
     * @brief Initialize session.
     * @param serverNodeName           Server node ID.
     * @param clientId               Session's client ID.
     * @param cleanSession           Clean session flag.
     * @param onMessage              Callback executed for each loaded message.
     * @return Session init type: new or restored.
     */
    virtual SessionInitType initSession(const std::string& serverNodeName,
                                        const std::string& clientId, bool cleanSession, const std::function<void(const std::shared_ptr<MessageDelivery>&)>& onMessage);

    void pack(sptk::Buffer& buffer) override;
    void unpack(const sptk::Buffer& sourceData) override;
    void unpack(const sptk::Buffer& sourceData, std::string& nodeName, std::string& clientId, ProtocolVersion& protocolVersion, Destinations& destinations);
    void storeRecordAsync(const std::function<void()>& callback) override;

    [[nodiscard]] std::shared_ptr<sptk::RedisConnect> getRedisConnection() const
    {
        return m_redisConnection;
    }

    RecordId recordId() const override
    {
        return 0;
    }

    SStorage storage() const override
    {
        return nullptr;
    }

    void subscribedTo(const std::shared_ptr<Subscription>& subscription, Qos qos, SubscriptionOptions subscriptionOptions) override;
    void unsubscribedFrom(const Subscription& subscription) override;

private:
    std::shared_ptr<sptk::RedisConnect> m_redisConnection;   ///< Redis connection.
    std::optional<sptk::Variant>        m_prefetchedSession; ///< See setPrefetchedSession(); consumed by the next lookup.
};

} // namespace xmq
