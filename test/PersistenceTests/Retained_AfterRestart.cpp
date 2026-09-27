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

#include "common/mqtt/PublishMessage.h"
#include "storage/RedisStorage.h"
#include "test/PersistenceTests/PersistenceTests.h"
#include "test/TestServers.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr auto settleTime = 100ms;
constexpr auto deliveryTimeout = 2s;

shared_ptr<client::MqttClient> connectRetainClient(const string&                 clientId,
                                                   const PublishMessageCallback& messageCallback = nullptr)
{
    auto client = make_shared<client::MqttClient>(XMQ_PersistenceTests::logEngine());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    if (messageCallback)
    {
        client->onMessage(messageCallback);
    }

    EXPECT_EQ(ReasonCode::Success,
              client->connect(Host("localhost", XMQ_PersistenceTests::TestTcpPortNumber),
                              ConnectCredentials(clientId, "user", "secret"),
                              connectParameters, ProtocolVersion::MqttV5));

    return client;
}

} // namespace

// A retained message has to outlive the broker that holds it. It used to live on the Subscription
// object, and the subscription tree is rebuilt from each session's subscription list at startup -
// so after a restart the message was simply gone, with nothing in storage to restore it from.
TEST_F(XMQ_PersistenceTests, Retained_SurvivesServerRestart)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const String retainedPayload("retained across a restart");

    {
        const auto publisher = connectRetainClient(publisherClientId);
        publisher->publish(topicName, retainedPayload.c_str(), Qos::Qos1, true);
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    server = restartServer();

    // A brand new subscriber, so nothing here comes from a restored session - only from the
    // retained message the broker read back at startup.
    String    receivedPayload;
    Semaphore received;
    const auto subscriber = connectRetainClient(subscriberClientId,
                                                [&receivedPayload, &received](const SPublishMessage& message)
                                                {
                                                    receivedPayload.assign(bit_cast<const char*>(message->payloadData()),
                                                                           message->payloadSize());
                                                    received.post();
                                                });
    subscriber->subscribe(topicName);

    EXPECT_TRUE(received.wait_for(deliveryTimeout)) << "The retained message did not survive the restart";
    EXPECT_STREQ(retainedPayload.c_str(), receivedPayload.c_str());

    subscriber->disconnect();
    stopServers();
}

// A wildcard subscription matches many topics, and each of them may hold its own retained message.
// Keeping the payload on the Subscription gave the subscriber exactly one - whichever topic
// happened to be published last - and named it after the filter rather than the topic.
TEST_F(XMQ_PersistenceTests, Retained_WildcardGetsEveryMatchingTopic)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const list topicNames {topicName + "/one", topicName + "/two", topicName + "/three"};

    {
        const auto publisher = connectRetainClient(publisherClientId);
        for (const auto& retainedTopic: topicNames)
        {
            publisher->publish(retainedTopic, "retained in " + retainedTopic, Qos::Qos1, true);
        }
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    map<string, string> receivedByTopic;
    mutex               receivedMutex;
    Semaphore           received;

    const auto subscriber = connectRetainClient(subscriberClientId,
                                                [&receivedByTopic, &receivedMutex, &received](const SPublishMessage& message)
                                                {
                                                    {
                                                        const scoped_lock lock(receivedMutex);
                                                        receivedByTopic.emplace(string(message->destination()->fullName()),
                                                                                string(bit_cast<const char*>(message->payloadData()),
                                                                                       message->payloadSize()));
                                                    }
                                                    received.post();
                                                });
    subscriber->subscribe(topicName + "/+");

    for (size_t message = 0; message < topicNames.size(); ++message)
    {
        EXPECT_TRUE(received.wait_for(deliveryTimeout)) << "Only " << message << " retained messages arrived";
    }

    const scoped_lock lock(receivedMutex);
    EXPECT_EQ(topicNames.size(), receivedByTopic.size());
    for (const auto& retainedTopic: topicNames)
    {
        const auto delivered = receivedByTopic.find(retainedTopic);
        ASSERT_NE(receivedByTopic.end(), delivered) << "No retained message for " << retainedTopic;
        EXPECT_EQ("retained in " + retainedTopic, delivered->second);
    }

    subscriber->disconnect();
    stopServers();
}

// The shape a person tries first, and the one that failed in the field: a retained message on a
// two-level topic, collected by a one-level-up filter. The test above uses a deeper topic and
// passed while this did not, which is the whole reason it is written separately.
TEST_F(XMQ_PersistenceTests, Retained_ShallowWildcardMatchesTopic)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const String retainedPayload("retained under a shallow filter");
    const string retainedTopic("shallow/50");

    {
        const auto publisher = connectRetainClient(publisherClientId);
        publisher->publish(retainedTopic, retainedPayload.c_str(), Qos::Qos1, true);
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    for (const auto& filter: {string("shallow/+"), string("shallow/#"), string("#")})
    {
        String    receivedPayload;
        Semaphore received;
        const auto subscriber = connectRetainClient(subscriberClientId + filter,
                                                    [&receivedPayload, &received](const SPublishMessage& message)
                                                    {
                                                        receivedPayload.assign(bit_cast<const char*>(message->payloadData()),
                                                                               message->payloadSize());
                                                        received.post();
                                                    });
        subscriber->subscribe(filter);

        EXPECT_TRUE(received.wait_for(deliveryTimeout)) << "Nothing retained arrived for filter " << filter;
        EXPECT_STREQ(retainedPayload.c_str(), receivedPayload.c_str()) << "Wrong payload for filter " << filter;
        subscriber->disconnect();
    }

    // Leave nothing behind: a retained message outlives the test that made it, and the next test
    // to subscribe with '#' would collect it.
    {
        const auto publisher = connectRetainClient(publisherClientId + "-clear");
        publisher->publish(retainedTopic, string(), Qos::Qos1, true);
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    stopServers();
}

// Clearing a retained message has to persist as firmly as setting one. It did not: the retained
// count is restored as zero at startup, so decrementing it for the first cleared message threw,
// and the throw came before the store was updated - the message stayed in Redis and reappeared at
// the next restart, after the broker had already told subscribers it was gone.
TEST_F(XMQ_PersistenceTests, Retained_ClearedAfterRestartStaysCleared)
{
    stopServers();
    auto server = createServer(TestTcpPortNumber, TestSslPortNumber, TestServicePortNumber, true);

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();

    {
        const auto publisher = connectRetainClient(publisherClientId);
        publisher->publish(topicName, "retained until cleared", Qos::Qos1, true);
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    // The clear has to happen after a restart: only then is the message one the broker read back
    // from storage rather than one it still remembers setting.
    server = restartServer();

    {
        const auto publisher = connectRetainClient(publisherClientId + "-clear");
        publisher->publish(topicName, string(), Qos::Qos1, true);
        this_thread::sleep_for(settleTime);
        publisher->disconnect();
    }

    server = restartServer();

    Semaphore received;
    const auto subscriber = connectRetainClient(subscriberClientId,
                                                [&received](const SPublishMessage&)
                                                {
                                                    received.post();
                                                });
    subscriber->subscribe(topicName);

    EXPECT_FALSE(received.wait_for(deliveryTimeout)) << "A cleared retained message came back after a restart";

    subscriber->disconnect();
    stopServers();
}
