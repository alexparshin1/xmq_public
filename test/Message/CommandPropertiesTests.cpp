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

// The "-D" option, which for a long time was a help entry and nothing else: the three words were
// read from the argument queue without being taken off it, so they came out as one word repeated
// and were then rejected as unexpected arguments - and nothing anywhere turned the result into
// properties. These tests cover the half that was missing, and the two shapes the old pattern
// could not express.

#include "utilities/CommandProperties.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

TEST(XMQ_CommandProperties, KeepsConnectAndPublishApart)
{
    const CommandProperties properties({"connect:receive-maximum=10", "publish:message-expiry-interval=30"});

    const auto connect = properties.forCommand("connect");
    const auto publish = properties.forCommand("publish");
    ASSERT_TRUE(connect);
    ASSERT_TRUE(publish);

    int64_t value = 0;
    EXPECT_TRUE(connect->getProperty(Property::ReceiveMaximum, value));
    EXPECT_EQ(10, value);
    EXPECT_FALSE(connect->getProperty(Property::MessageExpiryInterval, value));

    EXPECT_TRUE(publish->getProperty(Property::MessageExpiryInterval, value));
    EXPECT_EQ(30, value);
    EXPECT_FALSE(publish->getProperty(Property::ReceiveMaximum, value));
}

// A user property is a pair, so its value is itself "name=value" - two '=' in one definition. The
// option's pattern used to reject that, which made the one property a load test most wants to send
// the one property it could not.
TEST(XMQ_CommandProperties, TakesAUserPropertyPair)
{
    const CommandProperties properties({"publish:user-property=alpha=beta"});

    const auto publish = properties.forCommand("publish");
    ASSERT_TRUE(publish);
    EXPECT_EQ("beta", publish->getUserProperty("alpha"));
}

TEST(XMQ_CommandProperties, NothingAskedForIsNothingSent)
{
    const CommandProperties properties({});

    EXPECT_FALSE(properties.forCommand("connect"))
        << "an empty set would still put a property block on the wire";
    EXPECT_FALSE(properties.forCommand("publish"));
}

TEST(XMQ_CommandProperties, RefusesWhatItCannotSend)
{
    EXPECT_THROW(CommandProperties({"connect:no-such-property=1"}), Exception)
        << "a misspelled property should be refused, not silently dropped";
    EXPECT_THROW(CommandProperties({"connect:receive-maximum=ten"}), Exception)
        << "a number property given a word should be refused";
    EXPECT_THROW(CommandProperties({"subscribe:receive-maximum=10"}), Exception)
        << "only connect and publish carry properties here";
    EXPECT_THROW(CommandProperties({"publish:user-property=alpha"}), Exception)
        << "a user property without a value is half a pair";
}

} // namespace
