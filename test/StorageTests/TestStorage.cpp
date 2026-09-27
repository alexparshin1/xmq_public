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

#include "TestStorage.h"

#include "service/CClientSessionInfo.h"
#include "storage/RedisStorage.h"

using namespace std;
using namespace sptk;

namespace xmq {

TestStorage::TestStorage(const SServer& server, const bool cleanSession)
    : m_server(server)
    , m_storage(server->getStorage())
{
    //m_server->setStorage(m_storage);
    createDefaultSession(cleanSession);
    loadSubscriptions();
}

void TestStorage::createDefaultSession(const bool cleanSession)
{
    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    connectMessageParameters->setClientId(format("client1-{}", DateTime::Now().sinceEpoch().count() % 1000));
    connectMessageParameters->m_cleanSession = cleanSession;
    connectMessageParameters->m_protocolVersion = ProtocolVersion::MqttV31;

    const auto connectProperties = make_shared<MessageProperties>();
    connectProperties->setProperty(Property::ReceiveMaximum, 128);
    connectProperties->setProperty(Property::AuthenticationMethod, "plain");
    m_session = ClientSession::factory(m_server.get(), connectMessageParameters, connectProperties);
    m_server->getClientSessionManager()->add(m_session);
}

void TestStorage::releaseDefaultSession()
{
    m_session->clearSession();
    m_server->getClientSessionManager()->remove(m_session);
    m_session.reset();
}

void TestStorage::connect() const
{
    m_storage->connect("");
}

void TestStorage::loadSubscriptions()
{
    //m_server->getSubscriptionManager()->load(m_server.get());
    //m_server->getClientSessionManager()->load(m_storage);
}

TestStorage::~TestStorage()
{
    m_server->getClientSessionManager()->clear();
}

size_t TestStorage::countSessionSubscriptions(const string& topicName, const SClientSession& session) const
{
    const auto testSession = session ? session : m_session;
    const auto clientId = testSession->getClientId();
    const auto redis = m_server->getRedisStorage()->getRedis();
    const auto sessionKey = format("session_{}", clientId);
    const auto sessionValue = redis->getValue(sessionKey);
    if (sessionValue.isNull())
    {
        return 0;
    }

    // The session is stored in the binary format produced by PersistentClientSession::pack().
    string          nodeName;
    string          packedClientId;
    ProtocolVersion protocolVersion;
    Destinations    destinations;
    testSession->unpack(sessionValue.asBuffer(), nodeName, packedClientId, protocolVersion, destinations);

    size_t subscriptionCount = 0;
    for (const auto& destination: destinations)
    {
        if (destination.m_topic->fullName() == topicName)
        {
            subscriptionCount++;
        }
    }

    return subscriptionCount;
}

} // namespace xmq
