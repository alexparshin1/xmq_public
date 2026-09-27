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


/**
 * @file UserDatabaseTests.cpp
 * @brief The user-database sample, driven through the host the way the broker drives it.
 *
 * The accounts are written by the broker's own UserStore and hashed by its own PasswordHash, and
 * read back by the extension's separate copy of the verifier. That is the point of these tests:
 * the two implementations of `pbkdf2-sha256$iterations$salt$hash` are made to agree here, on
 * verifiers one of them produced, so a change to either is a failing test rather than a broker
 * that quietly stops admitting anybody.
 */

#include "server/Extensions/ExtensionHost.h"
#include "common/DirectoryNames.h"
#include "server/Settings/PasswordHash.h"
#include "server/Settings/UserStore.h"

#include <gtest/gtest.h>
#include <sptk5/FileLogEngine.h>
#include <sptk5/db/AutoDatabaseConnection.h>
#include <sptk5/db/DatabaseConnectionPool.h>
#include <sptk5/db/Query.h>

#include <chrono>
#include <filesystem>
#include <future>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

filesystem::path testUserDatabaseLibrary()
{
#ifdef XMQ_TEST_USER_DATABASE_LIBRARY
    return {XMQ_TEST_USER_DATABASE_LIBRARY};
#else
    return {};
#endif
}

/// Few iterations on purpose: what is under test is the format and the answers, not the cost.
constexpr int testIterations = 1000;

} // namespace

class XMQ_UserDatabaseTests
    : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (testUserDatabaseLibrary().empty() || !filesystem::exists(testUserDatabaseLibrary()))
        {
            GTEST_SKIP() << "the user-database sample was not built";
        }

        m_file = filesystem::temp_directory_path() /
                 ("xmq-user-database-" + to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                  to_string(chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        m_uri = DirectoryNames::sqliteUri(m_file);

        UserStore store(make_shared<DatabaseConnectionPool>(m_uri, 2));
        store.createSchema();

        // Written by the broker, so the verifier under test is the one the broker really produces.
        store.addUser({.m_username = "dave",
                       .m_password = PasswordHash::hash("Sunflower7!", testIterations),
                       .m_enabled = true});
        store.addUser({.m_username = "erin",
                       .m_password = PasswordHash::hash("Marigold3?", testIterations),
                       .m_enabled = false});
        store.addUser({.m_username = "never-set", .m_password = String(), .m_enabled = true});

        // Kept aside for the outage test, which needs an account the verification cache has never
        // answered for: a cached admission is a record of a check that really happened, and the
        // extension honours it for a minute even when the store has gone.
        store.addUser({.m_username = "grace",
                       .m_password = PasswordHash::hash("Foxglove5#", testIterations),
                       .m_enabled = true});
    }

    void TearDown() override
    {
        error_code errorCode;
        filesystem::remove(m_file, errorCode);
    }

    ExtensionHost::Configured extension(const string& unknownUser = string()) const
    {
        // No address here any more: the broker gives it through the ABI, so a test sets it on the
        // host with userDatabaseUri() exactly as the server does.
        ExtensionHost::Configured configured {.m_name = "user-database",
                                              .m_library = testUserDatabaseLibrary()};
        if (!unknownUser.empty())
        {
            configured.m_settings["unknown_user"] = unknownUser;
        }
        return configured;
    }

    static ExtensionHost::AuthDecision ask(ExtensionHost& host, const string& username,
                                           const string& password)
    {
        promise<ExtensionHost::AuthDecision> decided;
        host.authenticate({.m_clientId = "a-client", .m_username = username, .m_password = password},
                          [&decided](const ExtensionHost::AuthDecision decision, const shared_ptr<AclGroup>&)
                          { decided.set_value(decision); });
        auto answer = decided.get_future();
        EXPECT_EQ(future_status::ready, answer.wait_for(chrono::seconds(5)));
        return answer.get();
    }

    FileLogEngine    m_logEngine {"user-database-tests.log"};
    filesystem::path m_file;
    String           m_uri;
};

TEST_F(XMQ_UserDatabaseTests, admitsAnAccountWhosePasswordVerifies)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});
    ASSERT_TRUE(host.authenticating());

    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, refusesEverythingElseAboutAnAccount)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});

    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "dave", "Sunflower8!"))
        << "a wrong password";
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "erin", "Marigold3?"))
        << "a disabled account, whose password is right";

    // An account whose password has never been set authenticates nobody - least of all the empty
    // password, which is what a client that sent none arrives with.
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "never-set", ""));
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "never-set", "anything"));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, anUnknownAccountIsRefused)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});

    // This extension is the account database, so a name it does not have is a name that does not
    // exist. An allow-list extension abstains here; this one cannot, or the broker would fall back
    // to its own accounts for exactly the clients this is supposed to be the authority on.
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "nobody", "Sunflower7!"));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, anUnknownAccountCanBeLeftToWhoeverIsNext)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension("abstain")});

    // For a broker stacking this with a directory: the accounts here are one of several places a
    // client may be known.
    EXPECT_EQ(ExtensionHost::AuthDecision::NotHandled, ask(host, "nobody", "Sunflower7!"));
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"))
        << "abstaining on strangers must not change the answer for an account it has";

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, anonymousIsLeftToTheBroker)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});

    // Whether a client with no name may connect is the broker's policy, configured there. Answering
    // it here would put one decision in two places.
    EXPECT_EQ(ExtensionHost::AuthDecision::NotHandled, ask(host, "", ""));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, saysTheDatabaseIsUnreachableRatherThanRefusing)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});
    ASSERT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"));

    // The table goes while the extension is running, which is what an outage looks like from inside
    // it: the connection is fine and the answer is not there.
    {
        DatabaseConnectionPool           pool(m_uri, 1);
        const AutoDatabaseConnection     connection(pool);
        Query                            drop(connection.connection(), "DROP TABLE xmq_user");
        drop.exec();
    }

    // Not Deny, which would tell a client with perfectly good credentials that they are wrong, and
    // not NotHandled, which would hand them to the broker's own accounts and make an outage look
    // like somebody else's user.
    //
    // Asked of grace rather than dave: dave was admitted above and the verification cache still
    // holds that answer, so he would be let in without the store being touched. That is deliberate
    // - a client verified a moment ago survives a brief outage - and it is not what this test is
    // about.
    EXPECT_EQ(ExtensionHost::AuthDecision::SubsystemError, ask(host, "grace", "Foxglove5#"));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, refusesToStartWithoutADatabase)
{
    ExtensionHost::Configured configured {.m_name = "user-database",
                                          .m_library = testUserDatabaseLibrary()};

    ExtensionHost host(m_logEngine, "test");
    host.start({configured});

    // An authentication extension with no database abstains on every client, which looks exactly
    // like working and is not.
    EXPECT_FALSE(host.authenticating());

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, aChangedPasswordTakesEffectWhenTheBrokerSaysTheAccountsChanged)
{
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});

    // Verified once, so the answer is in the cache. That cache is why this call exists: the KDF
    // cannot be paid per connection, so an admission is remembered - and a password changed a
    // moment later would otherwise go on working for as long as the entry lives.
    ASSERT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"));

    {
        UserStore store(make_shared<DatabaseConnectionPool>(m_uri, 2));
        auto      dave = store.findUser("dave");
        ASSERT_TRUE(dave.has_value());
        dave->m_password = PasswordHash::hash("Nasturtium9$", testIterations);
        store.updateUser(*dave);
    }

    // Still the old answer: the store has changed and nothing has said so.
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"))
        << "the cache is supposed to hold until it is told otherwise";

    host.accountsChanged();

    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "dave", "Sunflower7!"))
        << "the old password must stop working the moment the interface saves the new one";
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Nasturtium9$"));

    host.stop();
}

TEST_F(XMQ_UserDatabaseTests, movingToAnotherDatabaseTakesTheAccountsAlong)
{
    // An operator who edits the URI from the default SQLite file to a real server means to take
    // their accounts with them. Losing them is never what a changed URI was asking for, and there
    // is no other moment at which both stores are open.
    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});
    ASSERT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"));

    const auto moved = filesystem::temp_directory_path() /
                       ("xmq-user-database-moved-" +
                        to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".db");
    error_code errorCode;
    filesystem::remove(moved, errorCode);

    // The broker's address changes; the host tells the extension, which reopens the store. There
    // is no settings report to read any more - the accounts moved, not a setting.
    host.userDatabaseUri(DirectoryNames::sqliteUri(moved).c_str());

    // The same account, authenticated out of the new store: the verifier travelled intact, so a
    // password that worked before the move works after it.
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"))
        << "the accounts must have come across";
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "dave", "wrong-password"));

    // Disabled stays disabled: what was copied is the account, not permission to connect.
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "erin", "Marigold3?"));

    host.stop();
    filesystem::remove(moved, errorCode);
}

TEST_F(XMQ_UserDatabaseTests, aStoreThatAlreadyHasAccountsIsNotAddedTo)
{
    // The shared store a cluster authenticates against is populated by somebody else. Pointing a
    // node at it must not quietly push that node's local accounts into it.
    const auto other = filesystem::temp_directory_path() /
                       ("xmq-user-database-other-" +
                        to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".db");
    error_code errorCode;
    filesystem::remove(other, errorCode);
    const String otherUri(DirectoryNames::sqliteUri(other));
    {
        UserStore store(make_shared<DatabaseConnectionPool>(otherUri, 2));
        store.createSchema();
        store.addUser({.m_username = "frank",
                       .m_password = PasswordHash::hash("Bluebell1@", testIterations),
                       .m_enabled = true});
    }

    ExtensionHost host(m_logEngine, "test");
    host.userDatabaseUri(m_uri.c_str());
    host.start({extension()});
    ASSERT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "dave", "Sunflower7!"));

    host.userDatabaseUri(otherUri.c_str());

    // Its own account answers; the ones from the store left behind do not.
    EXPECT_EQ(ExtensionHost::AuthDecision::Allow, ask(host, "frank", "Bluebell1@"));
    EXPECT_EQ(ExtensionHost::AuthDecision::Deny, ask(host, "dave", "Sunflower7!"))
        << "a populated store must not acquire the accounts of the one it replaced";

    host.stop();
    filesystem::remove(other, errorCode);
}
