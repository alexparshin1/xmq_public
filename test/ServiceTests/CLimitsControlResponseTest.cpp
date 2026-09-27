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
#include "service/CLimitsControlResponse.h"
#include <gtest/gtest.h>
#include <sptk5/sptk.h>

using namespace xmq;
using namespace sptk;

TEST(CLimitsControlResponseTest, ConstructorWithElementName)
{
    CLimitsControlResponse response("testElement", true);
    EXPECT_STREQ(response.name().c_str(), "testElement");
    response.m_result.m_success = true;
    response.m_result.m_description = "Ok";
    EXPECT_NO_THROW(response.checkRestrictions());
}

TEST(CLimitsControlResponseTest, DefaultConstructor)
{
    CLimitsControlResponse response;
    response.m_result.m_success = true;
    response.m_result.m_description = "Ok";
    EXPECT_NO_THROW(response.checkRestrictions());
    EXPECT_STREQ(CLimitsControlResponse::classId().c_str(), "LimitsControlResponse");
}

TEST(CLimitsControlResponseTest, CopyConstructor)
{
    CLimitsControlResponse response1;
    response1.m_result.m_success = true;
    response1.m_result.m_description = "Success";
    CLimitsControlResponse response2(response1);
    EXPECT_STREQ(response2.m_result.m_description.asString().c_str(), "Success");
}

TEST(CLimitsControlResponseTest, MoveConstructor)
{
    CLimitsControlResponse response1;
    response1.m_result.m_success = true;
    response1.m_result.m_description = "Success";
    CLimitsControlResponse response2(std::move(response1));
    EXPECT_STREQ(response2.m_result.m_description.asString().c_str(), "Success");
}
