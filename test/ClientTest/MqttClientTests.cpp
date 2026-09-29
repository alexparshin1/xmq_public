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

#include "MqttClientTests.h"
#include "client/MqttClient.h"
#include "common/DirectoryNames.h"
#include "test/TestServers.h"
#include <gtest/gtest.h>
#include <sptk5/StreamLogEngine.h>

using namespace std;
using namespace sptk;
using namespace xmq;

INSTANTIATE_TEST_SUITE_P(
    ProtocolVersions, XMQ_MqttClientTests,
    testing::Values(ProtocolVersion::MqttV31, ProtocolVersion::MqttV311, ProtocolVersion::MqttV5));

shared_ptr<Host> XMQ_MqttClientTests::m_mqttHost;
shared_ptr<Host> XMQ_MqttClientTests::m_mqttSslHost;
stringstream     XMQ_MqttClientTests::m_logStream;

shared_ptr<LogEngine> XMQ_MqttClientTests::createLogEngine(const LogPriority minPriority, const string& kind)
{
    m_logStream.str("");
    shared_ptr<LogEngine> logEngine;
    if (kind == "file")
    {
        logEngine = make_shared<FileLogEngine>(DirectoryNames::tempDirectory() / "xmq_client_tests.log");
    }
    else
    {
        logEngine = make_shared<StreamLogEngine>(m_logStream);
    }

    logEngine->reset();
    logEngine->option(LogEngine::Option::DATE, true);
    logEngine->option(LogEngine::Option::MILLISECONDS, true);
    logEngine->option(LogEngine::Option::STDOUT, true);
    logEngine->minPriority(minPriority);

    return logEngine;
}

void XMQ_MqttClientTests::SetUp()
{
    static constexpr uint16_t MqttTcpPortNumber = 1884;
    static constexpr uint16_t MqttSslPortNumber = 8884;
    // By name, like Redis: the one Mosquitto these tests use runs on the farm host, and every
    // machine that runs them - the build containers, the FreeBSD and Windows machines - resolves
    // mosquitto_server to it. getServerHost() used the name only when something listened on the
    // same port locally, so everywhere else these tests knocked on 127.0.0.1 and found nothing.
    m_mqttHost = make_shared<Host>("mosquitto_server", MqttTcpPortNumber);
    m_mqttSslHost = make_shared<Host>("mosquitto_server", MqttSslPortNumber);
}

TEST_F(XMQ_MqttClientTests, mosquittoReconnect)
{
    const auto logEngine = createLogEngine(LogPriority::Debug, "stream");

    client::MqttClient       client(logEngine);
    const ConnectCredentials credentials {"test_client", "user", "secret"};

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    auto rc = client.connect(*m_mqttHost, credentials, connectParameters, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());

    rc = client.connect(*m_mqttHost, credentials, connectParameters, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());

    client.disconnect();
    this_thread::sleep_for(m_smallTimeout);

    Strings logContent(m_logStream.str(), "\n\r", Strings::SplitMode::ANYCHAR);
    EXPECT_EQ(2u, logContent.grep("Connected").size());
    EXPECT_EQ(2u, logContent.grep("Disconnected").size());
}

TEST_F(XMQ_MqttClientTests, mosquittoConnectInvalidHost)
{
    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient       client(logEngine);
    const ConnectCredentials credentials {"test_client", "user", "secret"};

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    auto       invalidHost = Host("localhost", 1800);
    const auto rc = client.connect(invalidHost, credentials, connectParameters, ProtocolVersion::MqttV5);
    EXPECT_NE(ReasonCode::Success, rc);
    EXPECT_FALSE(client.isConnected());
}

TEST_F(XMQ_MqttClientTests, mosquittoConnectLogging)
{
    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient client(logEngine);

    const ConnectCredentials credentials {"test_client", "user", "secret"};

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    const auto rc = client.connect(*m_mqttHost, credentials, connectParameters, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());
    EXPECT_STREQ("test_client", client.getClientId().data());
    EXPECT_STREQ("test_client ", client.prefix().c_str());
}

TEST_F(XMQ_MqttClientTests, mosquittoConnectTcp)
{
    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient client(logEngine);

    const ConnectCredentials credentials {"test_client", "user", "secret"};

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    const auto rc = client.connect(*m_mqttHost, credentials, connectParameters, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());
    EXPECT_STREQ("test_client", client.getClientId().data());
    EXPECT_STREQ("test_client ", client.prefix().c_str());
}

TEST_F(XMQ_MqttClientTests, mosquittoConnectSsl)
{
    const auto logEngine = createLogEngine(LogPriority::Info);

    // No client certificate: the pair this used to send was the one that shipped with the
    // package, and nothing ships a private key any more. What the far end is verified against,
    // when it is, is a certificate imported from it.
    const auto sslKeys = make_shared<SSLKeys>();

    client::MqttClient       client(logEngine);
    const ConnectCredentials credentials {"test_client", "user", "secret"};

    const auto rc = client.connect(*m_mqttSslHost, credentials, {.m_cleanSession = true},
                                   ProtocolVersion::MqttV5, {}, sslKeys);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_F(XMQ_MqttClientTests, mosquittoSubscribeAndUnsubscribe)
{
    Semaphore subscribeSemaphore;
    Semaphore unsubscribeSemaphore;

    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient client(logEngine);

    client.onAck(
        [&subscribeSemaphore, &unsubscribeSemaphore](const SMessage& message)
        {
            if (message->is(Message::Type::SubscribeAck))
            {
                subscribeSemaphore.post();
            }
            else if (message->is(Message::Type::UnsubscribeAck))
            {
                unsubscribeSemaphore.post();
            }
        });

    const ConnectCredentials credentials {"test_client", "user", "secret"};
    const auto               rc = client.connect(*m_mqttHost, credentials, {.m_cleanSession = true}, ProtocolVersion::MqttV5);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(client.isConnected());

    const Destinations destinations {
        Destination(client::MqttClient::getTopic("topic1")),
        Destination(client::MqttClient::getTopic("topic2"))};

    client.subscribe(destinations, {});
    if (!subscribeSemaphore.wait_for(chrono::seconds(1)))
    {
        FAIL() << "Subscribe ACK didn't arrive";
    }

    client.unsubscribe(destinations);
    if (!unsubscribeSemaphore.wait_for(chrono::seconds(1)))
    {
        FAIL() << "Unsubscribe ACK didn't arrive";
    }

    client.disconnect();
    EXPECT_FALSE(client.isConnected());
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoLastWillAndTestament)
{
    const auto protocolVersion = GetParam();
    Semaphore  notificationSemaphore;

    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient notificationClient(logEngine);

    notificationClient.onMessage(
        [&notificationSemaphore](const SPublishMessage&)
        {
            notificationSemaphore.post();
        });

    const ConnectCredentials credentials {"notification_client", "user", "secret"};
    auto                     rc = notificationClient.connect(*m_mqttHost, credentials,
                                                             {.m_keepAliveInterval = m_sixtySeconds,
                                                              .m_cleanSession = true},
                                                             protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);

    const Destinations destinations {Destination(client::MqttClient::getTopic("notifications"))};

    notificationClient.subscribe(destinations);

    client::MqttClient client(logEngine);

    const auto               properties = make_shared<MessageProperties>();
    const ConnectCredentials credentials2 {"test_client", "user", "secret"};
    const auto               lastWill = make_shared<LastWillInfo>("notifications", "Client lost connection", false);
    rc = client.connect(*m_mqttHost,
                        credentials2,
                        {
                            .m_keepAliveInterval = m_sixtySeconds,
                            .m_lastWillInfo = lastWill,
                            .m_cleanSession = true,
                        },
                        protocolVersion, properties);
    EXPECT_EQ(ReasonCode::Success, rc);

    client.hangup();
    if (!notificationSemaphore.wait_for(chrono::seconds(1)))
    {
        FAIL() << "Last Will notification didn't arrive";
    }

    notificationClient.disconnect();
}

namespace {

void mqttClientTestsReceive(Qos qos, ProtocolVersion protocolVersion)
{
    constexpr size_t messageCount = 3;
    size_t           receivedMessagesCount = 0;
    Semaphore        semaphore;

    auto logEngine = XMQ_MqttClientTests::createLogEngine(LogPriority::Info);

    client::MqttClient subscriber(logEngine);

    subscriber.onMessage(
        [&semaphore, &receivedMessagesCount](const SPublishMessage&)
        {
            ++receivedMessagesCount;
            if (receivedMessagesCount == messageCount)
            {
                semaphore.post();
            }
        });

    const ConnectCredentials credentials {"test_subscriber_" + to_string(static_cast<int>(qos)), "user", "secret"};
    auto                     rc = subscriber.connect(*XMQ_MqttClientTests::m_mqttHost, credentials,
                                                     {.m_keepAliveInterval = XMQ_MqttClientTests::m_sixtySeconds,
                                                      .m_cleanSession = true},
                                                     protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(subscriber.isConnected());

    const Destinations destinations {Destination(client::MqttClient::getTopic("topic1")), Destination(client::MqttClient::getTopic("topic2"))};
    subscriber.subscribe(destinations);
    this_thread::sleep_for(XMQ_MqttClientTests::m_smallTimeout);

    client::MqttClient sender(logEngine);

    const ConnectCredentials credentials2 {"test_sender", "user", "secret"};
    rc = sender.connect(*XMQ_MqttClientTests::m_mqttHost, credentials2,
                        {
                            .m_keepAliveInterval = XMQ_MqttClientTests::m_sixtySeconds,
                            .m_cleanSession = true,
                        },
                        protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(sender.isConnected());

    for (size_t i = 0; i < messageCount; ++i)
    {
        sender.publish("topic1", "This is a test message " + to_string(i), qos);
    }

    if (!semaphore.wait_for(3s))
    {
        FAIL() << "Received only " << receivedMessagesCount << " of expected " << messageCount << " messages.";
    }

    subscriber.disconnect();
    sender.disconnect();
    this_thread::sleep_for(XMQ_MqttClientTests::m_smallTimeout);
    EXPECT_FALSE(subscriber.isConnected());
    EXPECT_FALSE(sender.isConnected());
}
} // namespace

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoReceiveQOS0)
{
    const auto protocolVersion = GetParam();
    mqttClientTestsReceive(Qos::Qos0, protocolVersion);
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoReceiveQOS1)
{
    const auto protocolVersion = GetParam();
    mqttClientTestsReceive(Qos::Qos1, protocolVersion);
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoReceiveQOS2)
{
    const auto protocolVersion = GetParam();
    mqttClientTestsReceive(Qos::Qos2, protocolVersion);
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoPing)
{
    const auto protocolVersion = GetParam();
    Semaphore  pongReceived;

    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient sender(logEngine);

    sender.onAck(
        [&pongReceived](const SMessage& message)
        {
            if (message->is(Message::Type::PingResp))
            {
                pongReceived.post();
            }
        });

    const ConnectCredentials credentials {"publisher", "user", "secret"};
    const auto               rc = sender.connect(*m_mqttHost, credentials,
                                                 {.m_keepAliveInterval = m_sixtySeconds,
                                                  .m_cleanSession = true},
                                                 protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(sender.isConnected());

    sender.ping();

    pongReceived.wait_for(chrono::seconds(1));

    sender.disconnect();
}

/// Note: This test expects the mosquitto server at mosquitto_server:1884
TEST_P(XMQ_MqttClientTests, mosquittoRetain)
{
    const auto protocolVersion = GetParam();
    Semaphore  publishReceived;

    const auto logEngine = createLogEngine(LogPriority::Info);

    client::MqttClient sender(logEngine);

    const ConnectCredentials credentials {"test_sender", "user", "secret"};
    auto                     rc = sender.connect(*m_mqttHost, credentials,
                                                 {
                                                     .m_keepAliveInterval = m_sixtySeconds,
                                                     .m_cleanSession = true,
                             },
                                                 protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(sender.isConnected());

    sender.publish(client::MqttClient::getTopic("notifications"), Buffer(String("Retained testMessage")), Qos::Qos0, {}, true);

    client::MqttClient receiver(logEngine);

    receiver.onMessage(
        [&publishReceived](const SPublishMessage&)
        {
            publishReceived.post();
        });

    const ConnectCredentials credentials2 {"test_receiver", "user", "secret"};
    rc = receiver.connect(*m_mqttHost, credentials2, {
                                                         .m_keepAliveInterval = m_sixtySeconds,
                                                         .m_cleanSession = true,
                                                     },
                          protocolVersion);
    EXPECT_EQ(ReasonCode::Success, rc);
    EXPECT_TRUE(receiver.isConnected());

    receiver.subscribe({Destination(client::MqttClient::getTopic("notifications"))});

    EXPECT_TRUE(publishReceived.wait_for(chrono::seconds(1)));

    sender.disconnect();
    receiver.disconnect();
}
