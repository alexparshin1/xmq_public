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


#include "server/Settings/PasswordHash.h"

#include <gtest/gtest.h>

#include <chrono>
#include <set>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
// Cheap on purpose: these tests are about the shape of the answer, not the cost of getting it.
constexpr int testIterations = 1000;
} // namespace

TEST(XMQ_PasswordHashTests, acceptsThePasswordItWasMadeFrom)
{
    const auto stored = PasswordHash::hash("correct horse battery staple", testIterations);

    EXPECT_TRUE(PasswordHash::verify("correct horse battery staple", stored));
    EXPECT_FALSE(PasswordHash::verify("Correct horse battery staple", stored));
    EXPECT_FALSE(PasswordHash::verify("", stored));
}

// The point of a salt: two people who choose the same password must not be visibly the same in the
// table, and one cracked hash must not answer for both.
TEST(XMQ_PasswordHashTests, theSamePasswordHashesDifferentlyEveryTime)
{
    set<String> seen;
    for (auto attempt = 0; attempt < 5; ++attempt)
    {
        const auto stored = PasswordHash::hash("same password", testIterations);
        EXPECT_TRUE(seen.insert(stored).second) << "a salt was reused";
        EXPECT_TRUE(PasswordHash::verify("same password", stored));
    }
}

// Nothing of the password survives in what is stored - which is the whole reason for the change
// away from the JWT, whose payload held it in clear text.
TEST(XMQ_PasswordHashTests, storesNothingOfThePasswordItself)
{
    const auto stored = PasswordHash::hash("hunter2-and-a-distinctive-tail", testIterations);

    EXPECT_EQ(String::npos, stored.find("hunter2"));
    EXPECT_EQ(String::npos, stored.find("distinctive"));
    EXPECT_TRUE(stored.starts_with("pbkdf2-sha256$")) << stored;
}

// The cost and the algorithm travel with the password, so that either can change later and every
// stored password still says which it was made with.
TEST(XMQ_PasswordHashTests, carriesItsAlgorithmAndCost)
{
    const auto stored = PasswordHash::hash("whatever", 4321);

    EXPECT_TRUE(stored.starts_with("pbkdf2-sha256$4321$")) << stored;
    EXPECT_TRUE(PasswordHash::verify("whatever", stored)) << "the stored cost must be the one used";

    EXPECT_TRUE(PasswordHash::needsRehash(stored, 9999));
    EXPECT_FALSE(PasswordHash::needsRehash(stored, 4321));
}

// Anything that is not a verifier is refused rather than throwing: these strings arrive from a
// table that a person can edit, and a broker that dies on a bad row is worse than one that says no.
TEST(XMQ_PasswordHashTests, refusesRubbishInsteadOfThrowing)
{
    for (const auto* stored: {"", "plain text password", "pbkdf2-sha256", "pbkdf2-sha256$$$",
                              "pbkdf2-sha256$notanumber$aa$bb", "pbkdf2-sha256$1000$zz$bb",
                              "pbkdf2-sha256$1000$aa$b", "argon2id$1$aa$bb", "$$$"})
    {
        EXPECT_FALSE(PasswordHash::verify("anything", stored)) << "accepted '" << stored << "'";
        EXPECT_FALSE(PasswordHash::isHashed(stored)) << "called '" << stored << "' a hash";
    }

    // Broken is not the same as outdated: re-hashing an unreadable verifier would replace it with
    // one for whatever password was offered, which is a way in rather than a repair.
    EXPECT_FALSE(PasswordHash::needsRehash("plain text password"));
}

TEST(XMQ_PasswordHashTests, tellsAHashFromAClearTextPassword)
{
    EXPECT_TRUE(PasswordHash::isHashed(PasswordHash::hash("x", testIterations)));
    EXPECT_FALSE(PasswordHash::isHashed("x"));
}

// What a person is told when a password is refused. Everything it lacks, in one sentence: told one
// missing thing at a time, somebody makes five attempts at what could have been one.
TEST(XMQ_PasswordHashTests, saysEverythingThatIsMissingAtOnce)
{
    EXPECT_TRUE(PasswordHash::weakness("Str0ng!Enough").empty()) << "a good password was refused";

    const auto everything = PasswordHash::weakness("abc");
    EXPECT_NE(String::npos, everything.find("at least 8 characters"));
    EXPECT_NE(String::npos, everything.find("at least one uppercase letter"));
    EXPECT_NE(String::npos, everything.find("at least one digit"));
    EXPECT_NE(String::npos, everything.find("at least one punctuation character"));
    EXPECT_EQ(String::npos, everything.find("lowercase")) << "'abc' has lowercase letters";

    // Read as a sentence rather than as a list: "A, B and C", not "A, B, C".
    EXPECT_EQ("The password needs at least 8 characters, at least one uppercase letter, "
              "at least one digit and at least one punctuation character.", everything);

    // One at a time, so that each rule is known to be checked rather than inferred from the pile.
    EXPECT_EQ("The password needs at least one uppercase letter.", PasswordHash::weakness("longenough1!"));
    EXPECT_EQ("The password needs at least one lowercase letter.", PasswordHash::weakness("LONGENOUGH1!"));
    EXPECT_EQ("The password needs at least one digit.", PasswordHash::weakness("LongEnough!!"));
    EXPECT_EQ("The password needs at least one punctuation character.", PasswordHash::weakness("LongEnough11"));
    EXPECT_EQ("The password needs at least 8 characters.", PasswordHash::weakness("Sh0rt!"));

    // Two, which is where the "and" first shows.
    EXPECT_EQ("The password needs at least 8 characters and at least one digit.",
              PasswordHash::weakness("Short!"));
}
