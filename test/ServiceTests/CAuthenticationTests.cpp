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

#include "service/CAuthentication.h"
#include <gtest/gtest.h>

using namespace xmq;

class CAuthenticationTests : public testing::Test
{
protected:
    void SetUp() override
    {
        m_auth = std::make_shared<CAuthentication>();
    }

    void TearDown() override
    {
        m_auth.reset();
    }

    std::shared_ptr<CAuthentication> m_auth;
};

TEST_F(CAuthenticationTests, DefaultConstructor)
{
    EXPECT_NO_THROW({ CAuthentication auth; });
}

TEST_F(CAuthenticationTests, Constructor)
{
    const CAuthentication authentication("authentication", false);
    EXPECT_STREQ(authentication.name().c_str(), "authentication");
}


TEST_F(CAuthenticationTests, EmptyCredentials)
{
    EXPECT_TRUE(m_auth->m_users.empty());
    EXPECT_FALSE(m_auth->m_allow_anonymous);
}

TEST_F(CAuthenticationTests, CopyConstructor)
{
    m_auth->m_allow_anonymous = true;

    CUser user;
    user.m_username = "user";
    user.m_password = "secret";
    m_auth->m_users.push_back(user);

    EXPECT_NO_THROW({
        CAuthentication authCopy(*m_auth);
        EXPECT_EQ(true, authCopy.m_allow_anonymous.asBool());
        EXPECT_EQ(1U, authCopy.m_users.size());
        EXPECT_EQ(authCopy.m_users[0].m_username.asString(), user.m_username.asString());
    });
}
