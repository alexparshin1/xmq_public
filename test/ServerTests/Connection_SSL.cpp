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

#include "common/DirectoryNames.h"
#include "common/mqtt/PublishMessage.h"
#include "test/ServerTests/ExternalClient/ExternalClient.h"
#include "test/ServerTests/ServerTests.h"
#include "test/SubscribeAndWait.h"
#include "test/TestMqttClient.h"

using namespace std;
using namespace sptk;
using namespace xmq;

TEST_P(XMQ_ServerTests, Connection_SslClient)
{
    const auto protocolVersion = GetParam();
    const auto client = make_shared<client::MqttClient>();
    // No client certificate: the listener does not ask for one, and the pair this used to send
    // was the one that shipped with the package - which no longer exists, because a private key
    // every installation holds is a private key everyone holds.
    const auto sslKeys = make_shared<SSLKeys>();

    const auto testNames = makeTestNames();

    const ConnectCredentials credentials {testNames.m_subscriberClientId, "user", "secret"};
    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", TestSslPortNumber), credentials, {},
                              protocolVersion, {}, sslKeys));

    this_thread::sleep_for(TinyTimeout);

    client->disconnect();
}

TEST_P(XMQ_ServerTests, Connection_SslMosquitto)
{
#ifndef _WIN32
    const auto protocolVersion = GetParam();
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    const ExternalClient client(ExternalClient::ClientKind::Mosquitto, publisherClientId, protocolVersion,
                                Host("localhost", TestSslPortNumber),
                                ExternalClient::EncryptionMode::Tls);

    const auto publisher = client.startPublisher(topicName, Qos::Qos0, 1, {},
                                                 ExternalClient::OutputMode::Debug);

    EXPECT_EQ(0, publisher->wait());
#endif
}

// Everything above this line only opens an encrypted connection, or publishes one message through
// an external client. The plain listener has dozens of publish/subscribe tests behind it; the
// encrypted one had none of its own. That is why a change to the broker's read path that was right
// for a plain socket and wrong for an SSL one - OpenSSL holds decrypted bytes no descriptor can
// report, and SSLSocket::recvUnlocked waits 30 seconds rather than saying "nothing yet" - was
// caught on 2026-09-08 by a bridge test and by nothing else. These two carry traffic over TLS.

TEST_P(XMQ_ServerTests, Publish_OverTls)
{
    const auto protocolVersion = GetParam();
    const auto logger = debugLog(false);
    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    // Enough messages that the reader has to come back for more, and small enough that several
    // of them land in one TLS record - which is the case a byte count taken from the socket
    // cannot see.
    constexpr size_t testMessageCount {500};
    constexpr size_t testPayloadSize {512};

    Buffer testPayload;
    while (testPayload.bytes() < testPayloadSize)
    {
        testPayload.append("This is a test. ");
    }
    testPayload.bytes(testPayloadSize);

    const auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, protocolVersion,
                                                        SMessageProperties {}, TestSslPortNumber, true);
    ASSERT_TRUE(subscriber->isConnected());

    Semaphore allMessagesReceived;
    atomic_size_t receivedMessageCount {0};
    atomic_size_t wrongPayloadCount {0};
    subscriber->onMessage(
        [&](const SPublishMessage& publishMessage)
        {
            if (publishMessage->payload().size() != testPayloadSize)
            {
                ++wrongPayloadCount;
            }
            if (++receivedMessageCount == testMessageCount)
            {
                allMessagesReceived.post();
            }
        });

    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName)));
    this_thread::sleep_for(SmallTimeout);

    const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion,
                                                       SMessageProperties {}, TestSslPortNumber, true);
    ASSERT_TRUE(publisher->isConnected());

    // Counted so a failure says which half of the path lost the traffic: acks are what the broker
    // sends back for what it managed to read, deliveries are what the subscriber managed to read.
    Semaphore     allAcksReceived;
    atomic_size_t receivedAckCount {0};
    publisher->onAck(
        [&](const SMessage& message)
        {
            if (message->is(Message::Type::PublishAck) && ++receivedAckCount == testMessageCount)
            {
                allAcksReceived.post();
            }
        });

    publisher->publishMultiple(topicName, testPayload, testMessageCount, Qos::Qos1);

    EXPECT_TRUE(allAcksReceived.wait_for(30s)) << "publish acks received: " << receivedAckCount.load();
    EXPECT_TRUE(allMessagesReceived.wait_for(30s));
    EXPECT_EQ(testMessageCount, receivedMessageCount.load());
    EXPECT_EQ(0U, wrongPayloadCount.load());

    // Hung up, not disconnected: an abrupt close is what used to poison the next TLS session this
    // broker thread served, and leaving it here keeps that covered from a second direction.
    subscriber->hangup();
    publisher->disconnect();
}

TEST_P(XMQ_ServerTests, Publish_LargeMessageOverTls)
{
    const auto protocolVersion = GetParam();
    const auto logger = debugLog(false);
    // Client ids of this test's own, so a broker-side error names the connection that had it.
    const auto [testPublisherId, testSubscriberId, topicName] = makeTestNames();
    const auto publisherClientId = "tls-large-pub-" + testPublisherId;
    const auto subscriberClientId = "tls-large-sub-" + testSubscriberId;

    // A TLS record holds at most 16 KB, so this payload arrives as tens of them, and it is larger
    // than the read chunk either side uses: neither end can assume one read is the whole message.
    constexpr size_t testPayloadSize {512 * 1024};

    Buffer testPayload;
    while (testPayload.bytes() < testPayloadSize)
    {
        testPayload.append("This is a large test payload. ");
    }
    testPayload.bytes(testPayloadSize);

    const auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, protocolVersion,
                                                        SMessageProperties {}, TestSslPortNumber, true);
    ASSERT_TRUE(subscriber->isConnected());

    Semaphore messageReceived;
    string    receivedPayload;
    subscriber->onMessage(
        [&](const SPublishMessage& publishMessage)
        {
            receivedPayload.assign(publishMessage->payload());
            messageReceived.post();
        });

    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName)));
    this_thread::sleep_for(SmallTimeout);

    const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion,
                                                       SMessageProperties {}, TestSslPortNumber, true);
    ASSERT_TRUE(publisher->isConnected());

    Semaphore ackReceived;
    publisher->onAck(
        [&](const SMessage& message)
        {
            if (message->is(Message::Type::PublishAck))
            {
                ackReceived.post();
            }
        });

    publisher->publishMultiple(topicName, testPayload, 1, Qos::Qos1);

    EXPECT_TRUE(ackReceived.wait_for(30s)) << "the broker never acknowledged the published message";
    ASSERT_TRUE(messageReceived.wait_for(30s));
    EXPECT_EQ(testPayloadSize, receivedPayload.size());
    EXPECT_EQ(0, memcmp(testPayload.data(), receivedPayload.data(), min(testPayloadSize, receivedPayload.size())));

    subscriber->hangup();
    publisher->disconnect();
}

// Written for the defect found on 2026-09-08 and explained on 2026-09-09: a TLS session closed
// abruptly - no close_notify, which is what MqttClient::hangup() does - left "unexpected eof" in
// OpenSSL's per-thread error queue, and the next session that same broker thread served was told
// its own healthy socket had failed. The victim was two connections later, not the next one, and
// nothing in the logs pointed at the session that caused it.
TEST_P(XMQ_ServerTests, AbruptTlsCloseAndTheNextConnection)
{
    const auto protocolVersion = GetParam();
    const auto logger = debugLog(false);

    constexpr size_t rounds = 10;
    size_t           failedRound = 0;

    for (size_t round = 1; round <= rounds && failedRound == 0; ++round)
    {
        const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

        // A session that carries a little traffic and is then torn down abruptly.
        {
            const auto doomed = make_shared<TestMqttClient>(logEngine(), "doomed-" + subscriberClientId, true, false,
                                                            protocolVersion, SMessageProperties {}, TestSslPortNumber, true);
            ASSERT_TRUE(doomed->isConnected()) << "round " << round;
            doomed->hangup();
        }

        // And the connection that follows it, which must work.
        const auto subscriber = make_shared<TestMqttClient>(logEngine(), subscriberClientId, true, false, protocolVersion,
                                                            SMessageProperties {}, TestSslPortNumber, true);
        ASSERT_TRUE(subscriber->isConnected()) << "round " << round;

        Semaphore messageReceived;
        subscriber->onMessage([&](const SPublishMessage&) { messageReceived.post(); });
        ASSERT_TRUE(test::subscribeAndWait(subscriber, Destination(client::MqttClient::getTopic(topicName))))
            << "round " << round;

        const auto publisher = make_shared<TestMqttClient>(logEngine(), publisherClientId, true, false, protocolVersion,
                                                           SMessageProperties {}, TestSslPortNumber, true);
        ASSERT_TRUE(publisher->isConnected()) << "round " << round;
        Semaphore ackReceived;
        publisher->onAck([&](const SMessage& message)
                         { if (message->is(Message::Type::PublishAck)) ackReceived.post(); });
        publisher->publishMultiple(topicName, Buffer("payload"), 1, Qos::Qos1);

        // Both are checked: the ack says the broker still reads the publisher's connection, the
        // delivery says it still writes the subscriber's. The defect this test was written for
        // killed the first.
        EXPECT_TRUE(ackReceived.wait_for(5s)) << "no publish ack in round " << round;
        if (!messageReceived.wait_for(5s))
        {
            failedRound = round;
        }
        subscriber->disconnect();
        publisher->disconnect();
    }

    EXPECT_EQ(0U, failedRound) << "the connection after an abrupt TLS close heard nothing";
}
