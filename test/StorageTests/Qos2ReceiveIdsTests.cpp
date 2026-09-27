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

// Inbound QoS 2 receive-side bookkeeping (MQTT 5 "Method B").
//
// The broker delivers a QoS 2 PUBLISH on arrival and then remembers only its packet id until
// PUBREL retires it. The id is what makes a repeated PUBLISH recognisable, so it has to outlive
// the node that first saw it: a session that moves to another node mid-handshake must not deliver
// the same message twice. These tests cover the id set itself and its round trip through storage.

#include "StorageTests.h"
#include "server/ClientSession/ClientSession.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/**
 * @brief Reads the stored ids back, waiting for them to become what is expected.
 *
 * The hash write is issued without waiting, so the value arrives some time after the call that
 * caused it returns. This used to be a 300ms sleep, which is a guess about how busy the machine is;
 * shuffling the suite made the guess wrong often enough to fail the test every time, because the
 * work queued ahead of it depended on what had run before.
 *
 * Returns what it last read either way, so the caller's assertion reports the real contents rather
 * than a timeout.
 */
template<typename Redis>
auto waitForStoredIds(const Redis& redis, const String& key, const size_t expectedCount,
                      const chrono::milliseconds timeout = chrono::seconds(5))
{
    const auto deadline = chrono::steady_clock::now() + timeout;

    auto ids = redis->getHashKeys(key);
    while (ids.size() != expectedCount && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(10ms);
        ids = redis->getHashKeys(key);
    }
    return ids;
}

SClientSession makeSession(Server* server, const string& clientId)
{
    const auto connectMessageParameters = make_shared<ConnectMessageParameters>();
    connectMessageParameters->m_protocolVersion = ProtocolVersion::MqttV5;
    connectMessageParameters->m_cleanSession = false;
    connectMessageParameters->setClientId(clientId);
    return ClientSession::factory(server, connectMessageParameters, make_shared<MessageProperties>());
}

} // namespace

TEST_F(XMQ_StorageTests, Qos2Receive_SuppressesRepeatedPublish)
{
    const auto session = makeSession(server().get(), "qos2_dup_client");

    constexpr MessageId packetId = 41;

    // First PUBLISH for this id: deliver it.
    EXPECT_TRUE(session->beginQos2Receive(packetId));

    // The client repeats the PUBLISH because our PUBREC was lost. It must not be delivered again.
    EXPECT_FALSE(session->beginQos2Receive(packetId));
    EXPECT_FALSE(session->beginQos2Receive(packetId));

    // A different id in the same session is unaffected.
    EXPECT_TRUE(session->beginQos2Receive(packetId + 1));

    // Once PUBREL retires the id, the client is free to reuse it for a genuinely new message.
    session->endQos2Receive(packetId);
    EXPECT_TRUE(session->beginQos2Receive(packetId));
}

TEST_F(XMQ_StorageTests, Qos2Receive_EndIsIdempotent)
{
    const auto session = makeSession(server().get(), "qos2_end_client");

    constexpr MessageId packetId = 42;

    // PUBREL for an id this node never saw - the case a session takeover produces. It must be
    // harmless, because the caller still owes the client a PUBCOMP either way.
    EXPECT_NO_THROW(session->endQos2Receive(packetId));

    EXPECT_TRUE(session->beginQos2Receive(packetId));
    EXPECT_NO_THROW(session->endQos2Receive(packetId));
    EXPECT_NO_THROW(session->endQos2Receive(packetId));
}

TEST_F(XMQ_StorageTests, Qos2Receive_IdsPersistAndClear)
{
    const string clientId = "qos2_persist_client";
    const auto   session = makeSession(server().get(), clientId);
    const auto   redis = server()->getRedisStorage()->getRedis();
    const auto   qos2Key = format("session_{}_qos2", clientId);

    constexpr MessageId packetId = 43;

    EXPECT_TRUE(session->beginQos2Receive(packetId));

    auto storedIds = waitForStoredIds(redis, qos2Key, 1);
    ASSERT_EQ(1U, storedIds.size()) << "An unreleased QoS 2 packet id was not persisted";
    EXPECT_EQ(to_string(packetId), storedIds.front());

    // PUBREL retires it, and the record must go with it - otherwise a later reuse of the same id
    // would be mistaken for a duplicate and silently dropped.
    session->endQos2Receive(packetId);

    storedIds = waitForStoredIds(redis, qos2Key, 0);
    EXPECT_TRUE(storedIds.empty()) << "A released QoS 2 packet id outlived its PUBREL";
}

TEST_F(XMQ_StorageTests, Qos2Receive_RestoredIdsStillSuppress)
{
    const auto session = makeSession(server().get(), "qos2_restore_client");

    // Standing in for a session that arrived from another node mid-handshake.
    constexpr MessageId inheritedId = 44;
    constexpr MessageId freshId = 45;
    session->restoreQos2ReceiveIds({inheritedId});

    // The client repeats a PUBLISH the previous owner already delivered.
    EXPECT_FALSE(session->beginQos2Receive(inheritedId));

    // An id the previous owner never held is still new here.
    EXPECT_TRUE(session->beginQos2Receive(freshId));
}
