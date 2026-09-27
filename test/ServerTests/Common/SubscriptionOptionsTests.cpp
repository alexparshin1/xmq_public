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

#include "common/SubscriptionOptions.h"
#include <gtest/gtest.h>

using namespace xmq;

TEST(SubscriptionOptionsTests, DefaultConstructor)
{
    const SubscriptionOptions options;

    EXPECT_EQ(options.getQos(), Qos::Qos2);
    EXPECT_FALSE(options.getNoLocal());
    EXPECT_FALSE(options.getRetainAsPublished());
    EXPECT_EQ(options.getRetainHandling(), SubscribeRetainHandling::RetainAlways);
}

TEST(SubscriptionOptionsTests, ParameterizedConstructor)
{
    const SubscriptionOptions options(Qos::Qos2, SubscribeRetainHandling::DoNotRetain, true);

    EXPECT_EQ(options.getQos(), Qos::Qos2);
    EXPECT_TRUE(options.getRetainAsPublished());
    EXPECT_EQ(options.getRetainHandling(), SubscribeRetainHandling::DoNotRetain);
}

TEST(SubscriptionOptionsTests, CopyConstructor)
{
    const SubscriptionOptions original(Qos::Qos1, SubscribeRetainHandling::RetainIfNew, true);

    const SubscriptionOptions copy(original);

    EXPECT_EQ(copy.getQos(), original.getQos());
    EXPECT_EQ(copy.getNoLocal(), original.getNoLocal());
    EXPECT_EQ(copy.getRetainAsPublished(), original.getRetainAsPublished());
    EXPECT_EQ(copy.getRetainHandling(), original.getRetainHandling());
}

TEST(SubscriptionOptionsTests, CopyAssignment)
{
    const SubscriptionOptions original(Qos::Qos1, SubscribeRetainHandling::RetainIfNew, true);

    SubscriptionOptions assigned;
    assigned = original;

    EXPECT_EQ(assigned.getQos(), original.getQos());
    EXPECT_EQ(assigned.getNoLocal(), original.getNoLocal());
    EXPECT_EQ(assigned.getRetainAsPublished(), original.getRetainAsPublished());
    EXPECT_EQ(assigned.getRetainHandling(), original.getRetainHandling());
}

TEST(SubscriptionOptionsTests, GettersAndSetters)
{
    SubscriptionOptions options;

    options.setQos(Qos::Qos1);
    EXPECT_EQ(options.getQos(), Qos::Qos1);

    options.setNoLocal(true);
    EXPECT_TRUE(options.getNoLocal());

    options.setRetainAsPublished(true);
    EXPECT_TRUE(options.getRetainAsPublished());

    options.setRetainHandling(SubscribeRetainHandling::RetainIfNew);
    EXPECT_EQ(options.getRetainHandling(), SubscribeRetainHandling::RetainIfNew);
}

TEST(SubscriptionOptionsTests, QosLevels)
{
    using enum Qos;
    SubscriptionOptions options;

    options.setQos(Qos0);
    EXPECT_EQ(options.getQos(), Qos0);

    options.setQos(Qos1);
    EXPECT_EQ(options.getQos(), Qos1);

    options.setQos(Qos2);
    EXPECT_EQ(options.getQos(), Qos2);
}

TEST(SubscriptionOptionsTests, RetainHandlingOptions)
{
    SubscriptionOptions options;

    using enum SubscribeRetainHandling;
    options.setRetainHandling(RetainAlways);
    EXPECT_EQ(options.getRetainHandling(), RetainAlways);

    options.setRetainHandling(RetainIfNew);
    EXPECT_EQ(options.getRetainHandling(), RetainIfNew);

    options.setRetainHandling(DoNotRetain);
    EXPECT_EQ(options.getRetainHandling(), DoNotRetain);
}

TEST(SubscriptionOptionsTests, NoLocalFlag)
{
    SubscriptionOptions options;

    EXPECT_FALSE(options.getNoLocal());

    options.setNoLocal(true);
    EXPECT_TRUE(options.getNoLocal());

    options.setNoLocal(false);
    EXPECT_FALSE(options.getNoLocal());
}

TEST(SubscriptionOptionsTests, RetainAsPublishedFlag)
{
    SubscriptionOptions options;

    EXPECT_FALSE(options.getRetainAsPublished());

    options.setRetainAsPublished(true);
    EXPECT_TRUE(options.getRetainAsPublished());

    options.setRetainAsPublished(false);
    EXPECT_FALSE(options.getRetainAsPublished());
}

TEST(SubscriptionOptionsTests, toString)
{
    using enum SubscribeRetainHandling;

    EXPECT_EQ(toString(RetainAlways), "RetainAlways");
    EXPECT_EQ(toString(RetainIfNew), "RetainIfNew");
    EXPECT_EQ(toString(DoNotRetain), "RetainNever");
}
