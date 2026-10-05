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

#include "ExternalClient.h"
#include "test/ServerTests/ServerTests.h"

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

shared_ptr<client::MqttClient> makeSubscriber(const string& clientId, size_t totalSendMessageCount, size_t& receivedMessageCount,
                                              Semaphore& allMessagesReceived, const string& topicName)
{
    auto subscriber = make_shared<client::MqttClient>(XMQ_ServerTests::server()->getLogEngine());
    subscriber->onMessage([&receivedMessageCount, &allMessagesReceived, totalSendMessageCount](const SPublishMessage&)
                          {
                              receivedMessageCount++;
                              if (receivedMessageCount >= totalSendMessageCount)
                              {
                                  allMessagesReceived.post();
                              }
                          });

    constexpr size_t receiveMaximum = 1000;

    const ConnectCredentials subscriberCredentials(clientId, "user", "secret");

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = true;

    const auto connectMessageProperties = make_shared<MessageProperties>();
    connectMessageProperties->setProperty(Property::ReceiveMaximum, receiveMaximum);
    EXPECT_EQ(ReasonCode::Success,
              subscriber->connect(Host("localhost", XMQ_ServerTests::TestTcpPortNumber), subscriberCredentials,
                                  connectParameters, ProtocolVersion::MqttV5, connectMessageProperties));
    subscriber->subscribe(Destination(client::MqttClient::getTopic(topicName), SubscriptionOptions {Qos::Qos1}));

    return subscriber;
}

void connectAndSendTest(const ExternalClient::ClientKind externalClientKind)
{
    constexpr auto totalSendMessageCount = 30U;
    const auto [publisherClientId, subscriberClientId, topicName] = XMQ_ServerTests::makeTestNames();

    Semaphore allMessagesReceived;
    size_t    receivedMessageCount {0};
    auto      subscriber = makeSubscriber(subscriberClientId,
                                          totalSendMessageCount, receivedMessageCount, allMessagesReceived,
                                          topicName);

    this_thread::sleep_for(100ms);

    const ExternalClient client(externalClientKind, publisherClientId);

    const auto publisher = client.startPublisher(topicName, Qos::Qos1,
                                                 totalSendMessageCount, {},
                                                 ExternalClient::OutputMode::Debug, 128);
    this_thread::sleep_for(100ms);

    EXPECT_EQ(0, publisher->wait());
    EXPECT_TRUE(allMessagesReceived.wait_for(1s));
    EXPECT_EQ(totalSendMessageCount, receivedMessageCount);
}

void sendAndReceiveTest(const ExternalClient::ClientKind externalClientKind, const size_t messageSize = 128)
{
    constexpr auto totalSendMessageCount = 10;
    const auto [publisherClientId, subscriberClientId, topicName] = XMQ_ServerTests::makeTestNames();

    const ExternalClient client1(externalClientKind, publisherClientId);
    const ExternalClient client2(externalClientKind, subscriberClientId);

    const auto subscriber = client2.startSubscriber(topicName, Qos::Qos1,
                                                    totalSendMessageCount, ExternalClient::SessionMode::Clean, {},
                                                    ExternalClient::OutputMode::Quiet, 10s);
    this_thread::sleep_for(100ms);

    const auto publisher = client1.startPublisher(topicName, Qos::Qos1,
                                                  totalSendMessageCount, {},
                                                  ExternalClient::OutputMode::Quiet, messageSize);

    EXPECT_EQ(0, publisher->wait());
    EXPECT_EQ(0, subscriber->wait());
}

} // namespace

TEST_F(XMQ_ServerTests, ExternalClient_Mosquitto_ConnectAndSend)
{
    connectAndSendTest(ExternalClient::ClientKind::Mosquitto);
}

TEST_F(XMQ_ServerTests, ExternalClient_XMQ_ConnectAndSend)
{
    connectAndSendTest(ExternalClient::ClientKind::XMQ);
}

TEST_F(XMQ_ServerTests, ExternalClient_Mosquitto_SendAndReceive)
{
    sendAndReceiveTest(ExternalClient::ClientKind::Mosquitto);
}

TEST_F(XMQ_ServerTests, ExternalClient_Mosquitto_SendAndReceiveLargeMessages)
{
    constexpr auto messageSize = 32768;
    sendAndReceiveTest(ExternalClient::ClientKind::Mosquitto, messageSize);
}

TEST_F(XMQ_ServerTests, ExternalClient_XMQ_SendAndReceive)
{
    sendAndReceiveTest(ExternalClient::ClientKind::XMQ);
}

TEST_F(XMQ_ServerTests, ExternalClient_XMQ_SendAndReceiveLargeMessages)
{
    constexpr auto messageSize = 4096;
    sendAndReceiveTest(ExternalClient::ClientKind::XMQ, messageSize);
}
