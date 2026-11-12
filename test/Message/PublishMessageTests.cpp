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

#include "base/Topic.h"
#include "common/mqtt/PublishMessage.h"

#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

TEST(PublishMessageTests, rejectsTopicLengthBeyondPacketInsteadOfReadingPastIt)
{
    const auto topicManager = make_shared<TopicManager>();
    mqtt::PublishMessage::TopicCache topicCache;

    // The topic length says 0x6c74, as happened when an HTTP Upgrade was sent to a plain MQTT
    // listener. Treat it as a malformed PUBLISH instead of passing an invalid string_view into
    // TopicManager's hash table.
    const uint8_t payload[] = {0x6c, 0x74, 'f', 'n', 'A', 'r', 'k'};
    const uint8_t fixedHeader[] = {0x30, static_cast<uint8_t>(sizeof(payload))};
    Packet packet(fixedHeader, sizeof(fixedHeader), payload, sizeof(payload));
    mqtt::FixedHeader header{};
    header.m_type = 3;
    header.m_qos = Qos::Qos0;

    EXPECT_THROW(mqtt::PublishMessage(topicManager, topicCache, header, std::move(packet),
                                      ProtocolVersion::MqttV311), sptk::Exception);
}

TEST(PublishMessageTests, rejectsTruncatedTopicLength)
{
    const auto topicManager = make_shared<TopicManager>();
    mqtt::PublishMessage::TopicCache topicCache;

    const uint8_t fixedHeader[] = {0x30, 1};
    Packet packet(fixedHeader, sizeof(fixedHeader), 1);
    mqtt::FixedHeader header{};
    header.m_type = 3;
    header.m_qos = Qos::Qos0;

    EXPECT_THROW(mqtt::PublishMessage(topicManager, topicCache, header, std::move(packet),
                                      ProtocolVersion::MqttV311), sptk::Exception);
}
