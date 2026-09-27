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
#include "service/CBridgeControl.h"
#include <gtest/gtest.h>
#include <sptk5/Exception.h>

using namespace sptk;
using namespace xmq;

// Test default constructor
TEST(CBridgeControlTests, DefaultConstructor)
{
    const CBridgeControl bridgeControl;
    EXPECT_EQ(CBridgeControl::classId(), "BridgeControl");
    EXPECT_STREQ(bridgeControl.name().c_str(), "bridge_control");
    EXPECT_FALSE(bridgeControl.isOptional());
}

// Test constructor with custom parameters
TEST(CBridgeControlTests, CustomConstructor)
{
    const CBridgeControl bridgeControl("custom_bridge_control", true);
    EXPECT_STREQ(bridgeControl.name().c_str(), "custom_bridge_control");
    EXPECT_TRUE(bridgeControl.isOptional());
}

// Test copy constructor
TEST(CBridgeControlTests, CopyConstructor)
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

    CBridgeControl bridgeControl1;
    bridgeControl1.m_action = "add";
    bridgeControl1.m_bridge = bridge1;

    const CBridgeControl bridgeControl2(bridgeControl1);

    CBridge bridge2(bridgeControl2.m_bridge);
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
TEST(CBridgeControlTests, MoveConstructor)
{
    CBridge bridge1;
    bridge1.m_id = 99;
    bridge1.m_node_name = "moving_node";
    bridge1.m_username = "mover";

    CBridgeControl bridgeControl1;
    bridgeControl1.m_action = "add";
    bridgeControl1.m_bridge = bridge1;

    const CBridgeControl bridgeControl2(std::move(bridgeControl1));

    CBridge bridge2(bridgeControl2.m_bridge);
    EXPECT_EQ(bridge2.m_id.value().asInteger(), 99);
    EXPECT_EQ(bridge2.m_node_name.value().asString(), "moving_node");
    EXPECT_EQ(bridge2.m_username.value().asString(), "mover");
}

TEST(CBridgeControlTests, LoadRestrictions)
{
    xdoc::Document doc;
    CBridgeControl bridgeControl("bridge_control", true);

    doc.root()->pushValue("action", "add", xdoc::Node::Type::Text);
    EXPECT_NO_THROW(bridgeControl.load(doc.root()));

    doc.root()->findFirst("action")->set("test");
    EXPECT_THROW(bridgeControl.load(doc.root()), sptk::Exception);
}
