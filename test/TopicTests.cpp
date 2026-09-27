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

#include "base/Topic.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

TEST(XMQ_Topic, ctors)
{
    TopicManager topicManager;
    String       testName("devices/usb/1");

    const auto* topic1 = topicManager.getTopic(testName);
    EXPECT_EQ(testName, topic1->toString());
    EXPECT_EQ(3U, topic1->path().size());
    EXPECT_FALSE(topic1->isWildcard());
    EXPECT_EQ(string("devices"), topic1->path()[0]);
    EXPECT_EQ(string("usb"), topic1->path()[1]);
    EXPECT_EQ(string("1"), topic1->path()[2]);

    String      testName2 = testName.replace("usb", "#");
    const auto* topic2 = topicManager.getTopic(testName2);
    EXPECT_NE(testName2, topic2->toString());
    EXPECT_EQ(2U, topic2->path().size());
    EXPECT_TRUE(topic2->isWildcard());
    EXPECT_EQ(string("devices"), topic2->path()[0]);
    EXPECT_EQ(string("#"), topic2->path()[1]);

    String      testName3 = testName.replace("usb", "+");
    const auto* topic3 = topicManager.getTopic(testName3);
    EXPECT_EQ(testName3, topic3->toString());
    EXPECT_EQ(3U, topic3->path().size());
    EXPECT_TRUE(topic3->isWildcard());
    EXPECT_EQ(string("devices"), topic3->path()[0]);
    EXPECT_EQ(string("+"), topic3->path()[1]);
    EXPECT_EQ(string("1"), topic3->path()[2]);
}

TEST(XMQ_Topic, flags)
{
    TopicManager topicManager;

    const auto* topic = topicManager.getTopic("topic/1");
    EXPECT_FALSE(topic->isWildcard());
    EXPECT_FALSE(topic->isSystem());
    EXPECT_FALSE(topic->isCluster());
    EXPECT_FALSE(topic->isShared());

    topic = topicManager.getTopic("topic/#");
    EXPECT_TRUE(topic->isWildcard());
    EXPECT_FALSE(topic->isSystem());
    EXPECT_FALSE(topic->isCluster());
    EXPECT_FALSE(topic->isShared());

    topic = topicManager.getTopic("topic/+/1");
    EXPECT_TRUE(topic->isWildcard());
    EXPECT_FALSE(topic->isSystem());
    EXPECT_FALSE(topic->isCluster());
    EXPECT_FALSE(topic->isShared());

    topic = topicManager.getTopic("$share/topic/1");
    EXPECT_FALSE(topic->isWildcard());
    EXPECT_FALSE(topic->isSystem());
    EXPECT_FALSE(topic->isCluster());
    EXPECT_TRUE(topic->isShared());

    topic = topicManager.getTopic("$SYS/version");
    EXPECT_FALSE(topic->isWildcard());
    EXPECT_TRUE(topic->isSystem());
    EXPECT_FALSE(topic->isCluster());
    EXPECT_FALSE(topic->isShared());

    topic = topicManager.getTopic("$CLUSTER/notify");
    EXPECT_FALSE(topic->isWildcard());
    EXPECT_FALSE(topic->isSystem());
    EXPECT_TRUE(topic->isCluster());
    EXPECT_FALSE(topic->isShared());
}

TEST(XMQ_Topic, invalidTopics)
{
    TopicManager topicManager;

    const Strings invalidTopics {"topic#/1", "/topic/1", "#topic/1", "+topic/1", "$topic/1", "topic//1"};
    for (const auto& invalidTopic: invalidTopics)
    {
        try
        {
            topicManager.getTopic(invalidTopic);
            FAIL() << "Expected exception for invalid topic: " << invalidTopic;
        }
        catch (const Exception& e)
        {
            EXPECT_TRUE(e.message().contains("Invalid topic name"));
        }
    }
}

TEST(XMQ_Topic, performance)
{
    TopicManager topicManager;

    Strings          topicNames;
    constexpr size_t maxTopics = 1000000;
    for (size_t index = 0; index < maxTopics; ++index)
    {
        topicNames.push_back("device/usb/" + to_string(index));
    }

    Stopwatch stopwatch;

    stopwatch.start();
    vector<const Topic*> topicVector;
    for (const auto& topicName: topicNames)
    {
        const auto topic = topicManager.getTopic(topicName);
        auto       topic2 = topicManager.getTopic(*topic);
        topicVector.push_back(topic2);
    }
    stopwatch.stop();
    COUT("Created " << maxTopics << " topics for " << stopwatch.milliseconds() << "ms (" << maxTopics / stopwatch.milliseconds() << "K/sec)");
    topicVector.clear();

    stopwatch.start();
    for (const auto& topicName: topicNames)
    {
        topicManager.getTopic(topicName);
    }
    stopwatch.stop();
    COUT("Made " << maxTopics << " topics for " << stopwatch.milliseconds() << "ms (" << maxTopics / stopwatch.milliseconds() << "K/sec)");

    // Calling the same topics but already cached by topic factory
    stopwatch.start();
    for (const auto& topicName: topicNames)
    {
        topicManager.getTopic(topicName);
    }
    stopwatch.stop();
    COUT("Used " << maxTopics << " topics for " << stopwatch.milliseconds() << "ms (" << maxTopics / stopwatch.milliseconds() << "K/sec)");
}

TEST(XMQ_Topic, createShared)
{
    TopicManager topicManager;

    const vector<string_view> expectedPath {"devices", "usb", "1"};

    const auto topic1 = topicManager.getTopic("devices/usb/1");
    EXPECT_FALSE(topic1->isShared());
    EXPECT_EQ(topic1->path(), expectedPath);

    const auto topic2 = topicManager.getTopic("$share/queue/devices/usb/1");
    EXPECT_TRUE(topic2->isShared());
    EXPECT_EQ(topic2->path(), expectedPath);
    EXPECT_STREQ(string(topic2->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic2->shareGroupName()).c_str(), "queue");
}

TEST(XMQ_Topic, copyCtor_Shared)
{
    TopicManager topicManager;

    const vector<string_view> expectedPath {"devices", "usb", "1"};

    auto topic1 = topicManager.getTopic("$share/queue/devices/usb/1");
    EXPECT_TRUE(topic1->isShared());
    EXPECT_EQ(topic1->path(), expectedPath);
    EXPECT_STREQ(string(topic1->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic1->shareGroupName()).c_str(), "queue");

    const auto topic2 = topicManager.getTopic(*topic1);
    EXPECT_TRUE(topic2->isShared());
    EXPECT_EQ(topic2->path(), expectedPath);
    EXPECT_STREQ(string(topic2->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic2->shareGroupName()).c_str(), "queue");

    EXPECT_TRUE(*topic1 == *topic2);

    EXPECT_TRUE(topic2->isShared());
    EXPECT_EQ(topic2->path(), expectedPath);
    EXPECT_STREQ(string(topic2->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic2->shareGroupName()).c_str(), "queue");
}

TEST(XMQ_Topic, moveCtor_Shared)
{
    TopicManager topicManager;

    const vector<string_view> expectedPath {"devices", "usb", "1"};

    const auto topic1 = topicManager.getTopic("$share/queue/devices/usb/1");
    EXPECT_TRUE(topic1->isShared());
    EXPECT_EQ(topic1->path(), expectedPath);
    EXPECT_STREQ(string(topic1->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic1->shareGroupName()).c_str(), "queue");

    const auto topic2 = topicManager.getTopic(std::move(*topic1));
    EXPECT_TRUE(topic2->isShared());
    EXPECT_EQ(topic2->path(), expectedPath);
    EXPECT_STREQ(string(topic2->fullName()).c_str(), "$share/queue/devices/usb/1");
    EXPECT_STREQ(string(topic2->shareGroupName()).c_str(), "queue");
}
