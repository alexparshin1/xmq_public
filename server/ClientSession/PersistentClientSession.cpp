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

#include "PersistentClientSession.h"
#include "ClientSession.h"
#include "Server.h"
#include "storage/MessageDeliveryPacker.h"
#include "storage/RedisStorage.h"

#include <future>
#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

// The Redis storage doesn't exist when persistence is disabled.
SRedisConnect redisConnection(const Server* server)
{
    const auto redisStorage = server->getRedisStorage();
    return redisStorage ? redisStorage->getRedis() : nullptr;
}

} // namespace

PersistentClientSession::PersistentClientSession(Server*                          server,
                                                 const SConnectMessageParameters& connectMessageParameters,
                                                 const SMessageProperties&        connectMessageProperties)
    : ClientSessionData(server, connectMessageParameters, connectMessageProperties)
    , PersistentObject(redisConnection(server))
{
}

void PersistentClientSession::redisStoreSession(SessionStoreType sessionStoreType)
{
    if (!m_redisConnection || m_released)
    {
        return;
    }

    const auto clientId = string(getClientId());
    const auto sessionKey = "session_" + clientId;

    Buffer sessionData;
    pack(sessionData);

    m_redisConnection->setValueAsync(sessionKey, sessionData,
                                     [this, clientId, sessionStoreType]
                                     {
                                         if (sessionStoreType == SessionStoreType::SessionDataAndNodeLink)
                                         {
                                             const auto& nodeName = getNodeName();
                                             const auto  nodeSessionsKey = "node_" + nodeName + "_sessions";
                                             m_redisConnection->addSetMembers(nodeSessionsKey, {clientId});
                                         }
                                     });
}

void PersistentClientSession::redisRemoveSession(const Buffer& sessionData, const bool removeSessionMessages)
{
    string              nodeName;
    string              clientId;
    ProtocolVersion     protocolVersion;
    vector<Destination> destinations;
    unpack(sessionData, nodeName, clientId, protocolVersion, destinations);

    // Delete session key, and the inbound QoS 2 ids that are meaningless without it:
    (void) m_redisConnection->deleteKeys({"session_" + clientId, "session_" + clientId + "_qos2"});

    if (removeSessionMessages)
    {
        // Delete session messages:
        const auto sessionMessagesKey = "session_" + clientId + "_messages";
        (void) m_redisConnection->deleteKeys({sessionMessagesKey});
    }

    if (!nodeName.empty())
    {
        const auto nodeSessionsKey = "node_" + nodeName + "_sessions";
        (void) m_redisConnection->deleteSetMembers(nodeSessionsKey, {clientId});
    }
}

void PersistentClientSession::redisRemoveSession(const string& clientId, bool removeSessionMessages)
{
    if (!m_redisConnection || m_released)
    {
        return;
    }
    const auto sessionKey = "session_" + clientId;
    m_redisConnection->getValueAsync({sessionKey},
                                     [this, removeSessionMessages](const Variant& sessionData)
                                     {
                                         if (sessionData.isNull())
                                         {
                                             return;
                                         }

                                         redisRemoveSession(sessionData.asBuffer(), removeSessionMessages);
                                     });
}

PersistentClientSession::SessionInitType
PersistentClientSession::initSession(const string&                                             serverNodeName,
                                     const std::string&                                        clientId,
                                     const bool                                                cleanSession,
                                     const function<void(const shared_ptr<MessageDelivery>&)>& onMessage)
{
    if (!onMessage)
    {
        throw Exception("Invalid (null) onMessage callback");
    }

    if (const auto redisStorage = server().getRedisStorage())
    {
        m_redisConnection = getRedis();
    }
    else
    {
        return SessionInitType::New;
    }

    // Find the session. A CONNECT has normally looked it up already, without any thread waiting
    // on Redis for it (Server::lookUpSessionThen()), and left the answer here. Otherwise it is
    // asked for now - through the pipeline, though this thread waits: a synchronous getValue() takes
    // the connection's request socket for a round trip of its own, one caller at a time, and with
    // appendfsync always every reply waits for Redis to flush its log.
    const auto sessionKey = "session_" + clientId;
    Variant    sessionInfo;
    if (m_prefetchedSession)
    {
        sessionInfo = std::move(*m_prefetchedSession);
        m_prefetchedSession.reset();
    }
    else
    {
        const auto lookup = std::make_shared<std::promise<Variant>>();
        auto       lookupResult = lookup->get_future();
        m_redisConnection->getValueAsync(sessionKey, [lookup](const Variant& value)
                                         {
                                             lookup->set_value(value);
                                         });
        // A failed command reaches the connection's error handler and never this callback.
        if (constexpr auto lookupTimeout = std::chrono::seconds(10);
            lookupResult.wait_for(lookupTimeout) != std::future_status::ready)
        {
            throw Exception("Redis did not answer the session lookup for " + clientId);
        }
        sessionInfo = lookupResult.get();
    }
    const auto sessionInitType = sessionInfo.isNull() ? SessionInitType::New : SessionInitType::Restored;

    const auto nodeSessionsKey = "node_" + serverNodeName + "_sessions";
    if (sessionInitType == SessionInitType::New)
    {
        if (m_redisConnection && !cleanSession)
        {
            // New session: create session data and include it in the node's session set - queued,
            // not waited for.
            //
            // This runs on an authentication thread, and nothing the client is owed depends on
            // these two writes: the CONNACK's session-present flag comes from the lookup above,
            // which is the one round trip that has to be synchronous. Waiting for them cost two
            // thirds of the thread's time per connection, and the pool in front of these threads
            // is bounded (MaxQueuedAuthentications): a 20K persistent-session run filled it in
            // half a second and the broker refused 6562 of 20000 connections outright, reporting
            // "Server not available" to each.
            //
            // Ordering is safe. Every later write for this session goes through this same
            // connection - redisStoreSession() uses setValueAsync() on it - and a connection
            // performs its queued operations in order on one worker thread, so a subscription
            // stored a millisecond later still lands after the session record it belongs to.
            Buffer sessionData;
            pack(sessionData);
            m_redisConnection->setValueAsync(sessionKey, sessionData);
            m_redisConnection->addSetMembersAsync(nodeSessionsKey, {clientId}, [](size_t)
                                                  {
                                                  });
        }
        return sessionInitType;
    }

    // Session exists, continue:

    string          oldNodeName;
    string          clientIdStr;
    ProtocolVersion protocolVersion;
    Destinations    destinations;
    unpack(sessionInfo.asBuffer(), oldNodeName, clientIdStr, protocolVersion, destinations);
    const auto oldNodeSessionsKey = "node_" + oldNodeName + "_sessions";

    if (cleanSession)
    {
        if (!oldNodeName.empty())
            try
            {
                redisRemoveSession(clientId, true);
            }
            catch (const RedisConnectException& e)
            {
                server().logMessage(LogSubject::SessionErrors, LogPriority::Error, e.what());
            }
        return SessionInitType::New;
    }

    try
    {
        // Register session with this server node, if necessary. Queued rather than waited for,
        // for the reason given above: three more blocking round trips on an authentication thread,
        // and the client's answer depends on none of them.
        if (oldNodeName != serverNodeName)
        {
            if (!oldNodeName.empty())
            {
                m_redisConnection->deleteSetMembersAsync(oldNodeSessionsKey, {clientId}, [](size_t)
                                                         {
                                                         });
            }
            m_redisConnection->addSetMembersAsync(nodeSessionsKey, {clientId}, [](size_t)
                                                  {
                                                  });

            Buffer sessionData;
            pack(sessionData);
            m_redisConnection->setValueAsync(sessionKey, sessionData);
        }
    }
    catch (const RedisConnectException& e)
    {
        server().logMessage(LogSubject::SessionErrors, LogPriority::Error, e.what());
    }

    const auto self = dynamic_pointer_cast<ClientSession>(shared_from_this());
    for (const auto& destination: destinations)
    {
        (void) server().subscribeClient(self, destination);
    }

    // Load session message records - all of this session's messages:
    const auto sessionMessagesHash = format("session_{}_messages", clientId);
    const auto sessionMessageMap = m_redisConnection->getHashValues(sessionMessagesHash);

    vector<RecordId> recordIds(sessionMessageMap.size());
    ranges::transform(sessionMessageMap | views::keys, recordIds.begin(),
                      [](const string& recordId)
                      {
                          return std::stoi(recordId);
                      });
    ranges::sort(recordIds);

    for (const auto recordId: recordIds)
    {
        const auto& record = sessionMessageMap.at(to_string(recordId));
        auto        messageDelivery = make_shared<MessageDelivery>(self, record.asBuffer());
        onMessage(messageDelivery);
    }

    // Restore the inbound QoS 2 packet ids still awaiting PUBREL. Without these, a PUBLISH the
    // client repeats after the session moves here would be delivered a second time.
    const auto        unreleasedKeys = m_redisConnection->getHashKeys(format("session_{}_qos2", clientId));
    vector<MessageId> unreleasedQos2Ids;
    unreleasedQos2Ids.reserve(unreleasedKeys.size());
    for (const auto& unreleasedKey: unreleasedKeys)
    {
        unreleasedQos2Ids.push_back(static_cast<MessageId>(stoul(unreleasedKey)));
    }
    self->restoreQos2ReceiveIds(unreleasedQos2Ids);

    return SessionInitType::Restored;
}

void PersistentClientSession::pack(Buffer& buffer)
{
    startPacking(buffer);

    // A copy, held for the whole pack. getClientId() returns a reference to a member whose
    // shared_lock is released before the caller ever reads through it, so a reconnect writing the
    // client id on another thread would free the characters mid-write.
    const std::string clientId = getClientId();

    write(getNodeName());
    write(clientId);
    write(static_cast<uint8_t>(getProtocolVersion()));

    // Read under the lock, not through getSubscribedTo(): that returns a reference to the map and
    // releases its lock before the caller ever walks it. Meanwhile clearSessionUnlocked() calls
    // m_subscribedTo.clear() while holding the same mutex, so an unlocked walk here iterates map
    // nodes as they are being freed - which corrupts the heap rather than failing cleanly.
    const std::shared_lock subscriptionsLock(m_mutex);

    const auto& subscriptions = getSubscribedToUnlocked();
    write(static_cast<uint16_t>(subscriptions.size()));
    for (const auto& [subscription, qos, subscriptionOptions]: subscriptions | views::values)
    {
        write(subscription->fullName());
        write(static_cast<uint8_t>(qos));
    }
}

void PersistentClientSession::unpack(const Buffer& sourceData)
{
    string          nodeName;
    string          clientId;
    ProtocolVersion protocolVersion;
    Destinations    destinations;
    unpack(sourceData, nodeName, clientId, protocolVersion, destinations);
    setNodeName(nodeName);
    setClientId(clientId);
    setProtocolVersion(protocolVersion);

    const auto self = dynamic_pointer_cast<ClientSession>(shared_from_this());
    for (const auto& destination: destinations)
    {
        (void) server().subscribeClient(self, destination);
    }
}

void PersistentClientSession::unpack(const Buffer& sourceData, std::string& nodeName, std::string& clientId,
                                     ProtocolVersion& protocolVersion, Destinations& destinations)
{
    startUnpacking({sourceData.c_str(), sourceData.size()});
    read(nodeName);
    read(clientId);
    read(*reinterpret_cast<uint8_t*>(&protocolVersion));

    destinations.clear();
    uint16_t subscriptionCount;
    read(subscriptionCount);
    for (size_t i = 0; i < subscriptionCount; ++i)
    {
        string subscriptionName;
        read(subscriptionName);
        uint8_t qos;
        read(qos);

        const auto* topic = server().getTopic(subscriptionName);
        destinations.emplace_back(topic, SubscriptionOptions(qos));
    }
}

void PersistentClientSession::storeRecordAsync(const function<void()>&)
{
    if (m_released)
    {
        return;
    }
    Buffer buffer;
    pack(buffer);
    if (const auto redis = getRedis())
    {
        redis->setValue("session_" + getClientId(), buffer);
    }
}

bool PersistentClientSession::load()
{
    const string clientId(getClientId());
    if (isCleanSession())
    {
        // A clean session keeps no persistent state, so there is nothing to load. Any stale session
        // left in Redis by a previous connection (possibly on another cluster node) is removed
        // asynchronously, off the connecting reactor thread, to avoid blocking on Redis round-trips.
        if (const auto redisStorage = server().getRedisStorage())
        {
            redisStorage->cleanupSessionAsync(clientId, server().getNodeName());
        }
        return false;
    }
    return initSession(clientId, isCleanSession());
}

void PersistentClientSession::persist()
{
    if (m_released)
    {
        return;
    }
    if (isCleanSession())
    {
        redisRemoveSession(getClientId(), true);
        return;
    }

    initSession(getClientId(), false);
}

void PersistentClientSession::unpersist()
{
    if (m_redisConnection)
    {
        redisRemoveSession(getClientId(), true);
    }
}

bool PersistentClientSession::initSession(const string& clientId, const bool cleanSession)
{
    auto persistentDataLoaded = false;
    try
    {
        const auto clientSession = dynamic_pointer_cast<ClientSession>(shared_from_this());

        const auto sessionInitType = initSession(server().getNodeName(), clientId, cleanSession, [this](const SMessageDelivery& messageDelivery)
                                                 {
                                                     SMessageDispatch messageDispatch = dynamic_pointer_cast<MessageDispatch>(messageDelivery);
                                                     restoreMessageDelivery(messageDispatch);
                                                 });


        if (sessionInitType == SessionInitType::Restored && !cleanSession)
        {
            persistentDataLoaded = sessionInitType == SessionInitType::Restored;
        }
    }
    catch (const Exception& exception)
    {
        server().logMessage(LogSubject::SessionErrors, LogPriority::Error, [message = exception.what()]
                            {
                                return format("PersistentClientSession::getPersistentData: {}", message);
                            });
    }

    return persistentDataLoaded;
}

void PersistentClientSession::subscribedTo(const std::shared_ptr<Subscription>& subscription, const Qos qos, const SubscriptionOptions subscriptionOptions)
{
    ClientSessionData::subscribedTo(subscription, qos, subscriptionOptions);
    redisStoreSession(SessionStoreType::SessionData);
}

void PersistentClientSession::unsubscribedFrom(const Subscription& subscription)
{
    ClientSessionData::unsubscribedFrom(subscription);
    redisStoreSession(SessionStoreType::SessionData);
}
