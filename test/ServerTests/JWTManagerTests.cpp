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

#include "server/JWTManager.h"
#include <chrono>
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;
using namespace sptk;

namespace {
String generateTestToken(JWTManager& jwtManager, const string& clientId = "test-user")
{
    CUser user;
    user.m_id = 123;
    user.m_username = clientId;
    user.m_password = "password";
    return jwtManager.generateToken(user);
}

String extractClientId(const JWTManager& jwtManager, const string& token)
{
    const auto jwt = jwtManager.decodeToken(token);
    const auto rootNode = jwt->grants.root();
    const auto userNode = rootNode->findFirst("user");
    const auto clientId = userNode->getString("username");
    return clientId;
}
} // namespace

TEST(JWTManagerTests, ValidateValidToken)
{
    JWTManager jwtManager {12h};

    const auto token = generateTestToken(jwtManager);

    EXPECT_FALSE(token.empty());
    EXPECT_NE(token.find('.'), std::string::npos);

    EXPECT_TRUE(jwtManager.validateToken(token));
}

TEST(JWTManagerTests, ValidateInvalidToken)
{
    JWTManager        jwtManager {12h};
    const std::string invalidToken = "invalid.token.signature";

    EXPECT_FALSE(jwtManager.validateToken(invalidToken));
}

TEST(JWTManagerTests, ValidateEmptyToken)
{
    JWTManager jwtManager {12h};
    EXPECT_FALSE(jwtManager.validateToken(""));
}

TEST(JWTManagerTests, ValidateExpiredToken)
{
    JWTManager shortLivedManager {1s};
    const auto token = generateTestToken(shortLivedManager);

    EXPECT_TRUE(shortLivedManager.validateToken(token));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    EXPECT_FALSE(shortLivedManager.validateToken(token));
}

TEST(JWTManagerTests, ExtractClientId)
{
    JWTManager jwtManager {12h};

    const auto token = generateTestToken(jwtManager);
    const auto clientId = extractClientId(jwtManager, token);

    EXPECT_STREQ("test-user", clientId.c_str());
}

TEST(JWTManagerTests, ExtractClientIdFromInvalidToken)
{
    const JWTManager  jwtManager {12h};
    const std::string invalidToken = "invalid.token.here";

    EXPECT_THROW(extractClientId(jwtManager, invalidToken), std::exception);
}

TEST(JWTManagerTests, TokenUniqueness)
{
    JWTManager jwtManager {12h};

    const auto token1 = generateTestToken(jwtManager);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto token2 = generateTestToken(jwtManager);

    EXPECT_NE(token1, token2);
}

TEST(JWTManagerTests, TokenWithSpecialCharactersInClientId)
{
    JWTManager jwtManager {12h};

    const auto clientId = "client@test.com:8080/path?query=value";
    const auto token = generateTestToken(jwtManager, clientId);

    EXPECT_TRUE(jwtManager.validateToken(token));
    EXPECT_EQ(extractClientId(jwtManager, token), clientId);
}

TEST(JWTManagerTests, MultipleClientsTokenValidation)
{
    JWTManager jwtManager {12h};

    const std::string client1 = "client-1";
    const std::string client2 = "client-2";
    const std::string client3 = "client-3";

    const auto token1 = generateTestToken(jwtManager, client1);
    const auto token2 = generateTestToken(jwtManager, client2);
    const auto token3 = generateTestToken(jwtManager, client3);

    EXPECT_TRUE(jwtManager.validateToken(token1));
    EXPECT_TRUE(jwtManager.validateToken(token2));
    EXPECT_TRUE(jwtManager.validateToken(token3));

    EXPECT_EQ(extractClientId(jwtManager, token1), client1);
    EXPECT_EQ(extractClientId(jwtManager, token2), client2);
    EXPECT_EQ(extractClientId(jwtManager, token3), client3);
}
