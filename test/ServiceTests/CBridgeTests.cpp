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
#include "service/CBridge.h"
#include <gtest/gtest.h>

using namespace xmq;

// Test default constructor
TEST(CBridgeTest, DefaultConstructor)
{
    CBridge bridge;
    EXPECT_STREQ(bridge.name().c_str(), "bridge");
    EXPECT_FALSE(bridge.isOptional());
}

// Test constructor with custom parameters
TEST(CBridgeTest, CustomConstructor)
{
    CBridge bridge("custom_bridge", true);
    EXPECT_STREQ(bridge.name().c_str(), "custom_bridge");
    EXPECT_TRUE(bridge.isOptional());
}

// Test copy constructor
TEST(CBridgeTest, CopyConstructor)
{
    CBridge bridge1;
    bridge1.m_id = 42;
    bridge1.m_node_name = "test_node";
    bridge1.m_enabled = true;
    bridge1.m_host_port = "localhost:1883";
    bridge1.m_username = "user";
    bridge1.m_password = "pass";
    bridge1.m_encrypted = true;
    bridge1.m_clean_session = false;
    bridge1.m_mode = "active";

    CBridge bridge2(bridge1);
    EXPECT_EQ(bridge2.m_id.value().asInteger(), 42);
    EXPECT_EQ(bridge2.m_node_name.value().asString(), "test_node");
    EXPECT_EQ(bridge2.m_enabled.value().asBool(), true);
    EXPECT_EQ(bridge2.m_host_port.value().asString(), "localhost:1883");
    EXPECT_EQ(bridge2.m_username.value().asString(), "user");
    EXPECT_EQ(bridge2.m_password.value().asString(), "pass");
    EXPECT_EQ(bridge2.m_encrypted.value().asBool(), true);
    EXPECT_EQ(bridge2.m_clean_session.value().asBool(), false);
    EXPECT_EQ(bridge2.m_mode.value().asString(), "active");
}

// Test move constructor
TEST(CBridgeTest, MoveConstructor)
{
    CBridge bridge1;
    bridge1.m_id = 99;
    bridge1.m_node_name = "moving_node";
    bridge1.m_username = "mover";

    CBridge bridge2(std::move(bridge1));
    EXPECT_EQ(bridge2.m_id.value().asInteger(), 99);
    EXPECT_EQ(bridge2.m_node_name.value().asString(), "moving_node");
    EXPECT_EQ(bridge2.m_username.value().asString(), "mover");
}

// Test copy assignment operator
TEST(CBridgeTest, CopyAssignment)
{
    CBridge bridge1;
    bridge1.m_id = 123;
    bridge1.m_node_name = "copy_node";

    CBridge bridge2;
    bridge2 = bridge1;
    EXPECT_EQ(bridge2.m_id.value().asInteger(), 123);
    EXPECT_EQ(bridge2.m_node_name.value().asString(), "copy_node");
}

// Test move assignment operator
TEST(CBridgeTest, MoveAssignment)
{
    CBridge bridge1;
    bridge1.m_id = 456;
    bridge1.m_node_name = "move_node";

    CBridge bridge2;
    bridge2 = std::move(bridge1);
    EXPECT_EQ(bridge2.m_id.value().asInteger(), 456);
    EXPECT_EQ(bridge2.m_node_name.value().asString(), "move_node");
}
