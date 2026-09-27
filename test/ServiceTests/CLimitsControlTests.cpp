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
#include "service/CLimitsControl.h"
#include <gtest/gtest.h>
#include <sptk5/sptk.h>

using namespace xmq;
using namespace sptk;

class CLimitsControlTest : public testing::Test
{
protected:
    void SetUp() override
    {
        // Setup code if needed
    }

    void TearDown() override
    {
        // Cleanup code if needed
    }
};

TEST_F(CLimitsControlTest, ConstructorWithElementName)
{
    CLimitsControl control("testElement", true);
    EXPECT_THROW(control.checkRestrictions(), Exception);
    control.m_action = "get";
    EXPECT_NO_THROW(control.checkRestrictions());
}

TEST_F(CLimitsControlTest, FieldNamesElements)
{
    const Strings& elements = CLimitsControl::fieldNames(WSFieldIndex::Group::ELEMENTS);
    EXPECT_FALSE(elements.empty());
}

TEST_F(CLimitsControlTest, FieldNamesAttributes)
{
    const Strings& attributes = CLimitsControl::fieldNames(WSFieldIndex::Group::ATTRIBUTES);
    EXPECT_TRUE(attributes.empty());
}
