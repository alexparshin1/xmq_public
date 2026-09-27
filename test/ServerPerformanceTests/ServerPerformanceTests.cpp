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
#include "ServerPerformanceTests.h"
#include <sptk5/threads/JoiningThread.h>

using namespace std;
using namespace sptk;
using namespace xmq;
using namespace filesystem;

namespace {
vector<client::SMqttClient> makePublishers(const uint16_t portNumber, size_t totalSendMessageCount, const string& senderBaseName,
                                           const ProtocolVersion& protocolVersion, const SMessageProperties& connectMessageProperties,
                                           Semaphore& allMessagesSent, atomic_size_t& publishedMessageCount)
{
    vector<client::SMqttClient> publishers;

    for (size_t i = 0; i < XMQ_ServerPerformanceTests::TestSenderCount; ++i)
    {
        auto publisher = make_shared<client::MqttClient>(XMQ_ServerTests::server()->getLogEngine());

        stringstream str;
        str << senderBaseName << "_" << i;

        ConnectCredentials publisherCredentials {str.str(), "user", "secret"};
        publisher->onAck(
            [&publishedMessageCount, &allMessagesSent, totalSendMessageCount](const SMessage& message)
            {
                if (message->is(Message::Type::PublishAck) || message->is(Message::Type::PublishComplete))
                {
                    ++publishedMessageCount;
                    if (publishedMessageCount >= totalSendMessageCount)
                    {
                        allMessagesSent.post();
                    }
                }
            });

        constexpr size_t maxInflightMessages = 16384;
        EXPECT_EQ(ReasonCode::Success,
                  publisher->connect(Host("localhost", portNumber), publisherCredentials,
                                     {
                                         .m_maxInflightMessages = maxInflightMessages,
                                         .m_cleanSession = true,
                                     },
                                     protocolVersion, connectMessageProperties));
        publishers.push_back(publisher);
    }

    return publishers;
}
} // namespace

void XMQ_ServerPerformanceTests::testPublishPerformance(const string& serverName, uint16_t port, uint32_t messageCount, Qos qos,
                                                        ProtocolVersion protocolVersion, ExternalClient::EncryptionMode encryptionMode,
                                                        bool useSharedQueue)
{
    constexpr auto allowLostMessages = 0;
    const auto     totalSendMessageCount = messageCount * TestSenderCount;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto finalTopicName = useSharedQueue ? "$share/group1/" + topicName : topicName;

    client::MqttClient subscriber(server()->getLogEngine());
    size_t             receivedMessageCount = 0;
    uint64_t           totalTransferLatencyMcs = 0;
    Semaphore          allMessagesReceived;
    subscriber.onMessage([&receivedMessageCount, &allMessagesReceived, totalSendMessageCount, &totalTransferLatencyMcs](const SPublishMessage& message)
                         {
                             // The publisher stamps the sending time into the first payload bytes, so the
                             // transfer latency works for every protocol version, not just MQTT5 properties.
                             if (uint64_t sentTimestampMcs = 0;
                                 message->payloadSize() >= sizeof(sentTimestampMcs))
                             {
                                 memcpy(&sentTimestampMcs, message->payloadData(), sizeof(sentTimestampMcs));
                                 totalTransferLatencyMcs += LatencyTrace::now() - sentTimestampMcs;
                             }

                             receivedMessageCount++;
                             if (receivedMessageCount >= totalSendMessageCount)
                             {
                                 allMessagesReceived.post();
                             }
                         });

    constexpr size_t   receiveMaximum = 100000;
    ConnectCredentials subscriberCredentials {subscriberClientId, "user", "secret"};
    auto               connectMessageProperties = make_shared<MessageProperties>();
    connectMessageProperties->setProperty(Property::ReceiveMaximum, receiveMaximum);

    auto rc = subscriber.connect(Host("localhost", port), subscriberCredentials,
                                 {.m_cleanSession = true}, protocolVersion, connectMessageProperties);
    EXPECT_EQ(ReasonCode::Success, rc);
    ASSERT_TRUE(subscriber.isConnected());
    subscriber.subscribe(Destination(client::MqttClient::getTopic(finalTopicName), SubscriptionOptions {qos}));
    this_thread::sleep_for(100ms);

    Semaphore     allMessagesSent;
    atomic_size_t publishedMessageCount {0};

    vector<client::SMqttClient> publishers = makePublishers(port, totalSendMessageCount, publisherClientId,
                                                            protocolVersion, connectMessageProperties,
                                                            allMessagesSent, publishedMessageCount);

    for (auto const& publisher: publishers)
    {
        ASSERT_TRUE(publisher->isConnected());
    }

    Stopwatch sendStopwatch;
    Stopwatch totalStopwatch;
    sendStopwatch.start();
    totalStopwatch.start();

    vector<future<void>> publisherTasks;
    auto                 extraMessagesPerSender = allowLostMessages / TestSenderCount;
    for (auto const& publisher: publishers)
    {
        auto publisherTask = async(
            launch::async,
            [&publishedMessageCount, &publisher, &qos, extraMessagesPerSender, messageCount, &finalTopicName]
            {
                Buffer message;
                message.fill(0, 128);
                for (uint32_t i = 0; i < messageCount + extraMessagesPerSender; ++i)
                {
                    if (qos == Qos::Qos0)
                    {
                        ++publishedMessageCount;
                    }
                    const auto sendTimestampMcs = LatencyTrace::now();
                    memcpy(message.data(), &sendTimestampMcs, sizeof(sendTimestampMcs));
                    publisher->publish(client::MqttClient::getTopic(finalTopicName), message, qos, {}, false, false);
                }
            });
        publisherTasks.push_back(std::move(publisherTask));
    }

    if (qos != Qos::Qos0)
    {
        EXPECT_TRUE(allMessagesSent.wait_for(30s));
        sendStopwatch.stop();
    }

    for (auto const& publisherTask: publisherTasks)
    {
        publisherTask.wait();
    }

    if (qos == Qos::Qos0)
    {
        // No acks at QoS0: the send window ends when the last publish call returns.
        sendStopwatch.stop();
    }

    EXPECT_TRUE(allMessagesReceived.wait_for(30s));
    totalStopwatch.stop();

    for (const auto& publisher: publishers)
    {
        publisher->disconnect();
    }
    subscriber.disconnect();

    stringstream title;
    title << serverName << " QOS" << static_cast<int>(qos) << " " << (encryptionMode == ExternalClient::EncryptionMode::Tls ? "Tls" : "TCP")
          << " MQTT" << static_cast<int>(protocolVersion) << " Port " << port;
    printTitle(title.str());

    printTiming("Transferred           ", receivedMessageCount, totalStopwatch.seconds());
    if (receivedMessageCount > 0)
    {
        COUT("Transfer latency       " << fixed << setprecision(2)
                                       << static_cast<double>(totalTransferLatencyMcs) / static_cast<double>(receivedMessageCount) / 1E3 << " ms (avg)");
    }
}

double XMQ_ServerPerformanceTests::testSharedSubscriptionScaling(const uint16_t port, const uint32_t messageCount,
                                                                 const ProtocolVersion protocolVersion, const size_t subscriberCount)
{
    constexpr auto qos = Qos::Qos1;
    const auto     totalSendMessageCount = messageCount * TestSenderCount;

    const auto [publisherClientId, subscriberClientId, topicName] = makeTestNames();
    const auto sharedTopicName = "$share/group1/" + topicName;

    atomic_size_t receivedMessageCount {0};
    Semaphore     allMessagesReceived;

    constexpr size_t receiveMaximum = 10000;
    auto             connectMessageProperties = make_shared<MessageProperties>();
    connectMessageProperties->setProperty(Property::ReceiveMaximum, receiveMaximum);

    // Spread one logical consumer across N connections joined to the same shared group.
    vector<client::SMqttClient> subscribers;
    for (size_t i = 0; i < subscriberCount; ++i)
    {
        auto subscriber = make_shared<client::MqttClient>(server()->getLogEngine());
        subscriber->onMessage([&receivedMessageCount, &allMessagesReceived, totalSendMessageCount](const SPublishMessage&)
                              {
                                  if (++receivedMessageCount >= totalSendMessageCount)
                                  {
                                      allMessagesReceived.post();
                                  }
                              });
        ConnectCredentials subscriberCredentials {format("{}_{}", subscriberClientId, i), "user", "secret"};
        const auto         rc = subscriber->connect(Host("localhost", port), subscriberCredentials,
                                                    {.m_cleanSession = true}, protocolVersion, connectMessageProperties);
        EXPECT_EQ(ReasonCode::Success, rc);
        subscriber->subscribe(Destination(client::MqttClient::getTopic(sharedTopicName), SubscriptionOptions {qos}));
        subscribers.push_back(subscriber);
    }
    this_thread::sleep_for(100ms);

    Semaphore     allMessagesSent;
    atomic_size_t publishedMessageCount {0};

    vector<client::SMqttClient> publishers = makePublishers(port, totalSendMessageCount, publisherClientId,
                                                            protocolVersion, connectMessageProperties,
                                                            allMessagesSent, publishedMessageCount);

    constexpr auto messageSize = 128;
    Buffer         message;
    while (message.size() < messageSize)
    {
        message.append("This is a test message ");
    }
    message.bytes(messageSize);

    Stopwatch totalStopwatch;
    totalStopwatch.start();

    JoiningThreads publisherTasks;
    for (auto const& publisher: publishers)
    {
        publisherTasks.emplace_back([&publisher, messageCount, &sharedTopicName, &message]
                                    {
                                        for (uint32_t i = 0; i < messageCount; ++i)
                                        {
                                            publisher->publish(client::MqttClient::getTopic(sharedTopicName), message, Qos::Qos1);
                                        }
                                    });
    }

    EXPECT_TRUE(allMessagesReceived.wait_for(30s));
    totalStopwatch.stop();

    publisherTasks.clear();

    for (const auto& publisher: publishers)
    {
        publisher->disconnect();
    }
    for (const auto& subscriber: subscribers)
    {
        subscriber->disconnect();
    }

    stringstream title;
    title << "Shared subscription MQTT" << static_cast<int>(protocolVersion) << " subscribers=" << subscriberCount;
    printTitle(title.str());
    printTiming("Received ", receivedMessageCount, totalStopwatch.seconds());

    return static_cast<double>(receivedMessageCount) / totalStopwatch.seconds();
}
