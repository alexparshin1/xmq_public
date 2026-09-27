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

#include "service/CBridgeControlResponse.h"
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

TEST(CBridgeControlResponseTest, DefaultConstructor)
{
    CBridgeControlResponse response;
    EXPECT_STREQ(CBridgeControlResponse::classId().c_str(), "BridgeControlResponse");
}

TEST(CBridgeControlResponseTest, Constructor)
{
    const CBridgeControlResponse response("response", false);
    EXPECT_STREQ(response.name().c_str(), "response");
}

TEST(CBridgeControlResponseTest, CopyConstructor)
{
    CBridge bridge;
    bridge.m_id = 42;
    bridge.m_node_name = "test_node";
    bridge.m_enabled = true;

    CResult result;
    result.m_description = "Ok";
    result.m_success = true;

    CBridgeControlResponse original;
    original.m_list.push_back(bridge);
    original.m_result = result;

    CBridgeControlResponse copy(original);
    EXPECT_EQ(copy.m_list.size(), original.m_list.size());
    EXPECT_EQ(copy.m_list[0].m_id.asInteger(), original.m_list[0].m_id.asInteger());
    EXPECT_EQ(copy.m_list[0].m_node_name.asString(), original.m_list[0].m_node_name.asString());
    EXPECT_EQ(copy.m_result.m_description.asString(), original.m_result.m_description.asString());
    EXPECT_EQ(copy.m_result.m_success, original.m_result.m_success);
}

TEST(CBridgeControlResponseTest, MoveConstructor)
{
    CBridge bridge;
    bridge.m_id = 42;
    bridge.m_node_name = "test_node";
    bridge.m_enabled = true;

    CResult result;
    result.m_description = "Ok";
    result.m_success = true;

    CBridgeControlResponse original;
    original.m_list.push_back(bridge);
    original.m_result = result;

    CBridgeControlResponse moved(std::move(original));
    EXPECT_EQ(moved.m_list.size(), 1U);
    EXPECT_EQ(moved.m_list[0].m_id.asInteger(), 42);
    EXPECT_EQ(moved.m_list[0].m_node_name.asString(), "test_node");
    EXPECT_EQ(moved.m_result.m_description.asString(), "Ok");
    EXPECT_EQ(moved.m_result.m_success, true);
}
