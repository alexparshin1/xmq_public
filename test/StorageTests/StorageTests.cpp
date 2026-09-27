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

#include "StorageTests.h"
#include "TestStorage.h"
#include "base/FixedLengthQueue.h"
#include "common/mqtt/PublishMessage.h"
#include "storage/Storage.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

void XMQ_StorageTests::linkStorageQueueTests()
{
    // This method is only defined to force linking of the file
}

void XMQ_StorageTests::printStorageInfo(const string& connectString)
{
    String driverName;
    if (!connectString.empty())
    {
        const DatabaseConnectionString dcs(connectString);
        driverName = dcs.driverName() + "@" + dcs.hostName();
    }
    else
    {
        driverName = "memory";
    }

    stringstream ss;
    size_t       i = 0;
    const size_t offset = (80 - driverName.size() - 2) / 2;
    for (; i < offset; i++)
    {
        ss << "─";
    }
    ss << "[" << driverName << "]";
    i += driverName.size();

    for (; i < 80; i++)
    {
        ss << "─";
    }

    COUT(ss.str());
}

TEST_F(XMQ_StorageTests, FixedLengthQueue_single)
{
    constexpr size_t         queueSize = 10;
    constexpr auto           queueTimeout = chrono::milliseconds(10);
    FixedLengthQueue<size_t> queue(queueSize);
    for (size_t i = 0; i < queueSize; ++i)
    {
        if (!queue.push(i, queueTimeout))
        {
            FAIL() << "Queue is full";
        }
    }
    EXPECT_FALSE(queue.push(0, queueTimeout));

    for (size_t i = 0; i < queueSize; ++i)
    {
        size_t value {0};
        if (!queue.pop(value, queueTimeout))
        {
            FAIL() << "Queue is empty";
        }
        EXPECT_EQ(i, value);
    }
    EXPECT_TRUE(queue.empty());
}

namespace {
void insertIntegers(FixedLengthQueue<size_t>& queue, const size_t count, const chrono::milliseconds queueTimeout)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!queue.push(i, queueTimeout))
        {
            FAIL() << "Queue is full";
        }
    }
    EXPECT_FALSE(queue.push(0, queueTimeout));
}

void readIntegers(FixedLengthQueue<size_t>& queue, const size_t count, const chrono::milliseconds queueTimeout)
{
    size_t         index = 0;
    constexpr auto groupSize = 2;
    vector<size_t> values;
    values.reserve(groupSize);
    while (!queue.empty())
    {
        if (!queue.pop(values, groupSize, queueTimeout))
        {
            FAIL() << "Queue is empty";
        }
        EXPECT_EQ(2U, values.size());
        EXPECT_EQ(index++, values[0]);
        EXPECT_EQ(index++, values[1]);
    }

    EXPECT_EQ(count, index);
}
} // namespace

TEST_F(XMQ_StorageTests, FixedLengthQueue_group)
{
    constexpr size_t         queueSize = 10;
    FixedLengthQueue<size_t> queue(queueSize);
    insertIntegers(queue, queueSize, 10ms);
    readIntegers(queue, queueSize, 10ms);
}

vector<SMessageDelivery> XMQ_StorageTests::createTestMessageDeliveries(const size_t count, const Server* server, const TestStorage* storage, const size_t messageSize, const SClientSession& clientSession)
{
    Buffer sourceData(messageSize + 64);
    while (sourceData.size() < messageSize)
    {
        sourceData.append("This is a test data. ");
    }
    sourceData.bytes(messageSize);

    const auto payload = string_view(sourceData.c_str(), sourceData.size());

    vector<SMessageDelivery> messageDeliveries;
    messageDeliveries.reserve(count);
    const auto    session = clientSession ? clientSession : storage->session();
    atomic_size_t createdCounter {0};
    Semaphore     allCreated;
    for (uint16_t i = 1; i <= count; ++i)
    {
        const auto messageDelivery = createTestMessageDelivery(server, storage->session(), payload,
                                                               [&allCreated, &createdCounter, count](const SMessageDelivery&)
                                                               {
                                                                   auto counter = ++createdCounter;
                                                                   if (counter >= count)
                                                                   {
                                                                       allCreated.post();
                                                                   }
                                                               });
        messageDeliveries.push_back(messageDelivery);
    }

    allCreated.wait();

    return messageDeliveries;
}

SMessageDelivery XMQ_StorageTests::createTestMessageDelivery(const Server* server, const SClientSession& clientSession, string_view payload, const function<void(const SMessageDelivery&)>& completionCallback)
{
    const auto* topic = server->getTopic("test/topic");
    const auto  publish = make_shared<mqtt::PublishMessage>(topic, payload);
    publish->setSourceNode("primary");
    publish->setSender("client1");

    const auto properties = make_shared<MessageProperties>();
    properties->setProperty(Property::MaximumQOS, static_cast<uint8_t>(Qos::Qos2));
    properties->setProperty(Property::CorrelationData, "correlation_data");
    publish->setProperties(properties);

    const auto messageDelivery = MessageDelivery::create(clientSession, std::move(publish), Qos::Qos1,
                                                         0, SubscriptionIdSet {123, 456}, true, 0, completionCallback);

    return messageDelivery;
}
