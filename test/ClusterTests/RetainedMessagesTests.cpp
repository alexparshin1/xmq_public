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

RetainedMessages::Record record(const string& payload, const int64_t revision, const Qos qos = Qos::Qos1)
{
    return {.m_payload = payload, .m_qos = qos, .m_revision = revision};
}

optional<string> held(const RetainedMessages& messages, const string& topic)
{
    optional<string> payload;
    messages.forEachMatching(topic, [&payload](const string&, const RetainedMessages::Record& r) { payload = r.m_payload; });
    return payload;
}

} // namespace

using Change = RetainedMessages::Change;

/**
 * A node alone: a publication adds, replaces or clears, and repeating the same thing changes nothing.
 */
TEST(XMQ_RetainedMessages, aNodeAloneSetsReplacesAndClears)
{
    RetainedMessages messages;
    EXPECT_EQ(Change::None, messages.set("t", "", Qos::Qos1)) << "clearing what is not held";
    EXPECT_EQ(Change::Added, messages.set("t", "one", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.set("t", "one", Qos::Qos1));
    EXPECT_EQ(Change::Replaced, messages.set("t", "two", Qos::Qos1));
    EXPECT_EQ(Change::Removed, messages.set("t", "", Qos::Qos1));
    EXPECT_EQ(nullopt, held(messages, "t"));
    EXPECT_EQ(0U, messages.size());
}

/**
 * A committed change from elsewhere is taken only when its revision is greater: an equal one is a
 * repeat, a lower one is late, whatever order they arrive in.
 */
TEST(XMQ_RetainedMessages, remoteChangeIsTakenOnlyWhenNewer)
{
    RetainedMessages messages;
    bool             unknown = false;
    ASSERT_EQ(Change::Added, messages.applyRemote("t", record("five", 5), true, unknown));
    EXPECT_EQ(Change::None, messages.applyRemote("t", record("three", 3), false, unknown)) << "late";
    EXPECT_EQ(Change::None, messages.applyRemote("t", record("five", 5), false, unknown)) << "repeat";
    EXPECT_EQ("five", held(messages, "t"));
    EXPECT_EQ(Change::Replaced, messages.applyRemote("t", record("seven", 7), false, unknown));
    EXPECT_EQ("seven", held(messages, "t"));
}

/**
 * A clearing leaves nothing behind, so a late change to the topic cannot be told from a new one:
 * it is not believed, but reported as unknown, to be read from the storage.
 */
TEST(XMQ_RetainedMessages, lateChangeToAClearedTopicIsNotBelieved)
{
    RetainedMessages messages;
    bool             unknown = false;
    ASSERT_EQ(Change::Added, messages.applyRemote("t", record("held", 5), true, unknown));
    EXPECT_EQ(Change::Removed, messages.applyRemote("t", record("", 6), false, unknown));

    EXPECT_EQ(Change::None, messages.applyRemote("t", record("late", 4), false, unknown));
    EXPECT_TRUE(unknown);
    EXPECT_EQ(nullopt, held(messages, "t"));

    EXPECT_EQ(Change::None, messages.applyRemote("t", record("", 3), false, unknown)) << "a clearing of nothing";
    EXPECT_FALSE(unknown);

    // What the storage says is the authority.
    EXPECT_EQ(Change::Added, messages.applyRemote("t", record("stored", 8), true, unknown));
    EXPECT_EQ("stored", held(messages, "t"));
}

/**
 * A change published here shows at once; while its commit is out, a change from elsewhere is only
 * remembered, and when the commit is back the greater revision is what the topic shows.
 */
TEST(XMQ_RetainedMessages, localChangeWaitsForItsCommit)
{
    RetainedMessages messages;
    bool             unknown = false;

    // Committed after the other node's change: this node's wins.
    EXPECT_EQ(Change::Added, messages.takeLocal("a", "mine", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyRemote("a", record("theirs", 8), false, unknown));
    EXPECT_EQ("mine", held(messages, "a")) << "shown until the commit is back";
    EXPECT_EQ(Change::None, messages.applyCommit("a", record("mine", 9)));
    EXPECT_EQ("mine", held(messages, "a"));

    // Committed before it: the other node's wins.
    EXPECT_EQ(Change::Added, messages.takeLocal("b", "mine", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyRemote("b", record("theirs", 8), false, unknown));
    EXPECT_EQ(Change::Replaced, messages.applyCommit("b", record("mine", 7)));
    EXPECT_EQ("theirs", held(messages, "b"));
}

/**
 * Replacement racing a clearing, both ways round: the greater revision wins, and a clearing that
 * wins leaves nothing behind.
 */
TEST(XMQ_RetainedMessages, replacementRacingAClearingEndsInTheGreaterRevision)
{
    RetainedMessages messages;
    bool             unknown = false;

    // A clearing here, committed after another node's replacement.
    ASSERT_EQ(Change::Added, messages.applyRemote("a", record("old", 5), true, unknown));
    EXPECT_EQ(Change::Removed, messages.takeLocal("a", "", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyRemote("a", record("new", 6), false, unknown));
    EXPECT_EQ(Change::None, messages.applyCommit("a", record("", 7)));
    EXPECT_EQ(nullopt, held(messages, "a"));
    EXPECT_EQ(0U, messages.size());

    // A replacement here, committed before another node's clearing.
    ASSERT_EQ(Change::Added, messages.applyRemote("b", record("old", 5), true, unknown));
    EXPECT_EQ(Change::Replaced, messages.takeLocal("b", "mine", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyRemote("b", record("", 7), false, unknown));
    EXPECT_EQ(Change::Removed, messages.applyCommit("b", record("mine", 6)));
    EXPECT_EQ(nullopt, held(messages, "b"));
}

/**
 * Two clearings at once - one here, one elsewhere - clear the topic once.
 */
TEST(XMQ_RetainedMessages, twoClearingsClearOnce)
{
    RetainedMessages messages;
    bool             unknown = false;
    ASSERT_EQ(Change::Added, messages.applyRemote("t", record("held", 5), true, unknown));
    EXPECT_EQ(Change::Removed, messages.takeLocal("t", "", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyRemote("t", record("", 6), false, unknown));
    EXPECT_EQ(Change::None, messages.applyCommit("t", record("", 7)));
    EXPECT_EQ(nullopt, held(messages, "t"));
    EXPECT_EQ(0U, messages.size());
}

/**
 * Two changes published here before either commit is back: the topic shows the last of them, and
 * then the greatest revision.
 */
TEST(XMQ_RetainedMessages, changesInFlightTogetherEndInTheGreatestRevision)
{
    RetainedMessages messages;
    EXPECT_EQ(Change::Added, messages.takeLocal("t", "first", Qos::Qos1));
    EXPECT_EQ(Change::Replaced, messages.takeLocal("t", "second", Qos::Qos1));
    EXPECT_EQ(Change::None, messages.applyCommit("t", record("first", 3)));
    EXPECT_EQ("second", held(messages, "t"));
    EXPECT_EQ(Change::None, messages.applyCommit("t", record("second", 4)));
    EXPECT_EQ("second", held(messages, "t"));
}

/**
 * Reading a shard: what the storage holds is applied; a topic held here and absent there was
 * cleared - unless it was committed after the shard was numbered, or this node's change is in flight.
 */
TEST(XMQ_RetainedMessages, reconcileFollowsTheStorage)
{
    RetainedMessages messages;
    bool             unknown = false;

    // Topics of one shard, whatever their names hash to.
    vector<string> topics;
    for (int i = 0; topics.size() < 4; ++i)
    {
        if (const auto topic = format("t/{}", i); RetainedMessages::shardOf(topic) == 3)
        {
            topics.push_back(topic);
        }
    }
    const auto& cleared = topics[0];
    const auto& newer = topics[1];
    const auto& inFlight = topics[2];
    const auto& added = topics[3];

    ASSERT_EQ(Change::Added, messages.applyRemote(cleared, record("cleared", 3), true, unknown));
    ASSERT_EQ(Change::Added, messages.applyRemote(newer, record("newer", 9), true, unknown));
    ASSERT_EQ(Change::Added, messages.takeLocal(inFlight, "in flight", Qos::Qos1));

    map<string, Change> changes;
    messages.reconcile(3, 5, {{added, record("added", 4)}},
                       [&changes](const string& topic, const Change change) { changes[topic] = change; });

    EXPECT_EQ(nullopt, held(messages, cleared));
    EXPECT_EQ("newer", held(messages, newer));
    EXPECT_EQ("in flight", held(messages, inFlight));
    EXPECT_EQ("added", held(messages, added));
    EXPECT_EQ((map<string, Change> {{cleared, Change::Removed}, {added, Change::Added}}), changes);
}

/**
 * Every node puts a topic in the same shard, whatever its standard library hashes strings to.
 */
TEST(XMQ_RetainedMessages, shardsAreTheSameEverywhere)
{
    EXPECT_EQ(12U, RetainedMessages::shardOf("a")); // FNV-1a of "a" is 0xe40c292c
    for (const auto* topic: {"", "a/b", "sensors/42/temperature"})
    {
        EXPECT_LT(RetainedMessages::shardOf(topic), RetainedMessages::ShardCount);
    }
}

/**
 * The storage's form, "<revision>:<qos>:<payload>", is read back - a payload with colons and binary
 * bytes too - and a damaged value is refused.
 */
TEST(XMQ_RetainedMessages, sharedRecordsAreRead)
{
    RetainedMessages::Record read;
    ASSERT_TRUE(RetainedMessages::parseShared("12:1:a:b", read));
    EXPECT_EQ(12, read.m_revision);
    EXPECT_EQ(Qos::Qos1, read.m_qos);
    EXPECT_EQ("a:b", read.m_payload);

    ASSERT_TRUE(RetainedMessages::parseShared(string("7:2:\0\xff", 6), read));
    EXPECT_EQ(string("\0\xff", 2), read.m_payload);

    for (const auto* damaged: {"", ":1:x", "12:x", "12:3:x", "1a:1:x", "12:11:x"})
    {
        EXPECT_FALSE(RetainedMessages::parseShared(damaged, read)) << damaged;
    }
}

/**
 * What one node encodes, another decodes record for record, clearings and binary payloads included.
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
        EXPECT_EQ(r.m_revision, received[topic].m_revision) << topic;
    }

    // Cut short anywhere, the message is reported damaged rather than read past its end.
    EXPECT_FALSE(RetainedMessages::decode(string_view(message.c_str(), message.bytes() - 1), [](const string&, const auto&) {}));
}

/**
 * Records stored by an earlier version - the QoS, then the payload - are still read, as older than
 * any change made since; records in the current format come back as they went in.
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
    EXPECT_EQ(0, fromEarlier.m_revision);

    for (const auto& written: {record("value", 1234567, Qos::Qos2), record("", 99, Qos::Qos0)})
    {
        RetainedMessages::Record readBack;
        ASSERT_TRUE(RetainedMessages::unpack(RetainedMessages::pack(written), readBack));
        EXPECT_EQ(written.m_payload, readBack.m_payload);
        EXPECT_EQ(written.m_qos, readBack.m_qos);
        EXPECT_EQ(written.m_revision, readBack.m_revision);
    }
}
