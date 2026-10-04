/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "server/Subscription/RetainedMessages.h"

#include <gtest/gtest.h>

#include <map>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

RetainedMessages::Record record(const string& payload, const int64_t updated, const Qos qos = Qos::Qos1)
{
    return {.m_payload = payload, .m_qos = qos, .m_updated = updated};
}

optional<string> held(const RetainedMessages& messages, const string& topic)
{
    optional<string> payload;
    messages.forEachMatching(topic, [&payload](const string&, const RetainedMessages::Record& r) { payload = r.m_payload; });
    return payload;
}

} // namespace

/**
 * A change from another node is taken only when it is newer, whatever order the changes arrive in.
 */
TEST(XMQ_RetainedMessages, mergeTakesOnlyNewerChanges)
{
    RetainedMessages messages;
    EXPECT_EQ(RetainedMessages::Change::Added, messages.merge("a", record("new", 200)).change);
    EXPECT_FALSE(messages.merge("a", record("old", 100)).stored) << "an older change replaced a newer one";
    EXPECT_EQ("new", held(messages, "a"));

    EXPECT_EQ(RetainedMessages::Change::Replaced, messages.merge("a", record("newer", 300)).change);
    EXPECT_EQ("newer", held(messages, "a"));
}

/**
 * A clearing is kept as a tombstone, which an older copy of the message cannot override and a
 * newer publication can.
 */
TEST(XMQ_RetainedMessages, tombstoneOutranksOlderCopies)
{
    RetainedMessages messages;
    messages.merge("a", record("value", 100));
    EXPECT_EQ(RetainedMessages::Change::Removed, messages.merge("a", record("", 200)).change);
    EXPECT_EQ(nullopt, held(messages, "a"));
    EXPECT_EQ(0U, messages.size()) << "a tombstone was counted as a retained message";

    EXPECT_FALSE(messages.merge("a", record("value", 100)).stored) << "a stale copy brought a cleared message back";
    EXPECT_EQ(nullopt, held(messages, "a"));

    EXPECT_EQ(RetainedMessages::Change::Added, messages.merge("a", record("again", 300)).change);
    EXPECT_EQ("again", held(messages, "a"));
}

/**
 * Two changes made in the same millisecond are put in the same order on every node.
 */
TEST(XMQ_RetainedMessages, sameTimeChangesAreOrderedTheSameEverywhere)
{
    RetainedMessages first;
    first.merge("a", record("x", 100));
    first.merge("a", record("y", 100));

    RetainedMessages second;
    second.merge("a", record("y", 100));
    second.merge("a", record("x", 100));

    EXPECT_EQ(held(first, "a"), held(second, "a"));
}

/**
 * A publication on this node takes effect even when what it replaces carries a later time - a
 * node whose clock runs ahead must not make it lose - and it is stamped later than that.
 */
TEST(XMQ_RetainedMessages, localPublicationAlwaysWins)
{
    RetainedMessages messages;
    messages.merge("a", record("from a fast clock", 1000));

    const auto applied = messages.set("a", "local", Qos::Qos1, 500);
    EXPECT_TRUE(applied.stored);
    EXPECT_EQ("local", held(messages, "a"));
    EXPECT_GT(applied.record.m_updated, 1000);

    EXPECT_FALSE(messages.set("a", "local", Qos::Qos1, 600).stored) << "an identical publication was news";
}

/**
 * Clearing a topic this node holds nothing for still leaves a tombstone, because another node
 * may hold the message; clearing it again leaves nothing new.
 */
TEST(XMQ_RetainedMessages, clearingAnUnknownTopicLeavesATombstone)
{
    RetainedMessages messages;
    const auto       applied = messages.set("a", "", Qos::Qos0, 100);
    EXPECT_TRUE(applied.stored);
    EXPECT_TRUE(applied.record.isTombstone());
    EXPECT_EQ(RetainedMessages::Change::None, applied.change);

    EXPECT_FALSE(messages.set("a", "", Qos::Qos0, 200).stored);
    EXPECT_FALSE(messages.merge("a", record("late", 50)).stored);
}

/**
 * Tombstones go once they are older than their lifetime; messages never do.
 */
TEST(XMQ_RetainedMessages, expiredTombstonesArePurged)
{
    RetainedMessages messages;
    messages.merge("gone", record("", 100));
    messages.merge("kept", record("value", 100));

    EXPECT_TRUE(messages.purgeTombstones(100 + RetainedMessages::TombstoneLifetimeMs).empty());
    const auto purged = messages.purgeTombstones(101 + RetainedMessages::TombstoneLifetimeMs);
    ASSERT_EQ(1U, purged.size());
    EXPECT_EQ("gone", purged.front());
    EXPECT_EQ("value", held(messages, "kept"));
}

/**
 * What one node encodes, another decodes record for record, tombstones and binary payloads included.
 */
TEST(XMQ_RetainedMessages, encodedRecordsDecodeTheSame)
{
    const map<string, RetainedMessages::Record> sent {
        {"a/b", record("value", 123456789, Qos::Qos2)},
        {"cleared", record("", 42, Qos::Qos0)},
        {"binary", record(string("\0\x80\xff", 3), -1, Qos::Qos1)},
    };

    Buffer message;
    for (const auto& [topic, r]: sent)
    {
        RetainedMessages::encode(message, topic, r);
    }

    map<string, RetainedMessages::Record> received;
    EXPECT_TRUE(RetainedMessages::decode(string_view(message.c_str(), message.bytes()),
                                         [&received](const string& topic, const RetainedMessages::Record& r) { received[topic] = r; }));
    ASSERT_EQ(sent.size(), received.size());
    for (const auto& [topic, r]: sent)
    {
        EXPECT_EQ(r.m_payload, received[topic].m_payload) << topic;
        EXPECT_EQ(r.m_qos, received[topic].m_qos) << topic;
        EXPECT_EQ(r.m_updated, received[topic].m_updated) << topic;
    }

    // Cut short anywhere, the message is reported damaged rather than read past its end.
    EXPECT_FALSE(RetainedMessages::decode(string_view(message.c_str(), message.bytes() - 1), [](const string&, const auto&) {}));
}

/**
 * Records stored by an earlier version - the QoS, then the payload - are still read, as older than
 * any change made since; records in the current format come back as they went in, tombstones too.
 */
TEST(XMQ_RetainedMessages, storedRecordsOfBothFormatsAreRead)
{
    Buffer                   earlier;
    earlier.append(static_cast<char>(Qos::Qos1));
    earlier.append("old payload", 11);
    RetainedMessages::Record fromEarlier;
    ASSERT_TRUE(RetainedMessages::unpack(earlier, fromEarlier));
    EXPECT_EQ("old payload", fromEarlier.m_payload);
    EXPECT_EQ(Qos::Qos1, fromEarlier.m_qos);
    EXPECT_EQ(0, fromEarlier.m_updated);

    for (const auto& written: {record("value", 1234567, Qos::Qos2), record("", 99, Qos::Qos0)})
    {
        RetainedMessages::Record readBack;
        ASSERT_TRUE(RetainedMessages::unpack(RetainedMessages::pack(written), readBack));
        EXPECT_EQ(written.m_payload, readBack.m_payload);
        EXPECT_EQ(written.m_qos, readBack.m_qos);
        EXPECT_EQ(written.m_updated, readBack.m_updated);
    }
}
