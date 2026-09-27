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


#include "common/DirectoryNames.h"
#include "server/Settings/PasswordHash.h"
#include "server/Settings/UserManager.h"
#include "server/Settings/UserStore.h"
#include "test/TestServers.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Every database this build can reach, so the schema is proven where it will actually run rather
/// than only on the one that needs no server.
vector<pair<String, String>> reachableDatabases()
{
    vector<pair<String, String>> databases;

    const auto sqlite = filesystem::temp_directory_path() /
                        ("xmq_user_store_" + to_string(::getpid()) + ".db");
    filesystem::remove(sqlite);
    databases.emplace_back("SQLite3", DirectoryNames::sqliteUri(sqlite));

    vector<pair<String, String>> candidates {
        {"PostgreSQL", "postgresql://gtest:test#123@dbhost_pg/xmq_test"}};

#if !defined(_WIN32) || !defined(_DEBUG)
    // Not in a Windows debug build. MySQL ships its Windows client library release-only, so calling
    // it from a debug build crosses two C runtimes with two heaps: the failed connection this probe
    // is supposed to swallow corrupts the heap instead, and the process dies with 0xC0000374 in
    // SetUpTestSuite - before a single test has run, taking the whole suite with it.
    //
    // Skipped rather than caught, because it cannot be caught: the damage is done inside the
    // driver, and the exception never arrives.
    candidates.emplace_back("MySQL", "mysql://gtest:test#123@dbhost_mysql/xmq_test");
#endif

    for (const auto& [name, uri]: candidates)
    {
        try
        {
            DatabaseConnectionPool       pool(uri, 1);
            const AutoDatabaseConnection connection(pool);
            Query                        probe(connection.connection(), "SELECT 1");
            probe.open();
            probe.close();
            databases.emplace_back(name, uri);
        }
        catch (const Exception&)
        {
            // Not every developer's machine has one of these, and a schema test that cannot run is
            // not a schema test that failed.
        }
    }

    // Said out loud, because two of these are skipped when no server answers and a test that
    // quietly covered one database out of three would read exactly like one that covered all three.
    String covered;
    for (const auto& [name, uri]: databases)
    {
        covered += (covered.empty() ? "" : ", ") + name;
    }
    COUT("            Databases reached: " << covered << endl);

    return databases;
}

/// Nothing of an earlier run, so that "created" means created.
void dropEverything(DatabaseConnectionPool& pool)
{
    for (const auto* table: {"xmq_user_to_user_group", "xmq_user", "xmq_user_group", "xmq_store_version"})
    {
        try
        {
            const AutoDatabaseConnection connection(pool);
            Query                        drop(connection.connection(), String("DROP TABLE ") + table);
            drop.exec();
        }
        catch (const Exception&)
        {
            // Not there, which is where this wanted to get to.
        }
    }
}

/**
 * @brief One schema per test, on every database that answers, and none left behind.
 *
 * The servers are shared - PostgreSQL and MySQL both live on the build host, alongside the Redis
 * the whole farm uses - so tables left lying about are somebody else's problem later. Probing is
 * done once for the suite because it is three connections; the schema is made fresh for each test
 * because a test that inherits another's rows is a test that passes for the wrong reason.
 */
class XMQ_UserStoreTests
    : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        m_databases = reachableDatabases();
    }

    static void TearDownTestSuite()
    {
        for (const auto& [name, uri]: m_databases)
        {
            DatabaseConnectionPool pool(uri, 1);
            dropEverything(pool);
        }
        m_databases.clear();
    }

    void SetUp() override
    {
        ASSERT_FALSE(m_databases.empty()) << "no database answered, so nothing could be tested";

        for (const auto& [name, uri]: m_databases)
        {
            DatabaseConnectionPool pool(uri, 1);
            dropEverything(pool);
            UserStore(make_shared<DatabaseConnectionPool>(uri, 2)).createSchema();
        }
    }

    /// A store on one of the databases, with the schema already there.
    static UserStore storeOn(const String& uri)
    {
        return UserStore(make_shared<DatabaseConnectionPool>(uri, 2));
    }

    static std::vector<std::pair<String, String>> m_databases;
};

std::vector<std::pair<String, String>> XMQ_UserStoreTests::m_databases;

} // namespace

TEST_F(XMQ_UserStoreTests, createsItsSchemaOnEveryDatabaseWeSupport)
{
    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);

        EXPECT_EQ(1, store.version()) << name << ": a fresh store starts at version 1";

        // Called at every start, so it has to be safe to call when everything is already there.
        EXPECT_NO_THROW(store.createSchema()) << name << ": creating twice must be harmless";
    }
}

// The point of declaring foreign keys is that they are enforced, and both SQLite and MySQL have a
// way of accepting the declaration and enforcing nothing - SQLite until its pragma is set, MySQL
// whenever the constraint is written inside a column rather than beside it. Neither says so.
TEST_F(XMQ_UserStoreTests, membershipCannotPointAtAccountsThatDoNotExist)
{
    for (const auto& [name, uri]: m_databases)
    {
        DatabaseConnectionPool       pool(uri, 2);
        const AutoDatabaseConnection connection(pool);
        Query orphan(connection.connection(),
                     "INSERT INTO xmq_user_to_user_group (user_id, user_group_id) VALUES (4242, 4343)");

        EXPECT_THROW(orphan.exec(), Exception)
            << name << ": a membership row was accepted for a user and a group that do not exist";
    }
}

namespace {

// A users file as an older broker wrote one: the password inside a JWT, in the open.
filesystem::path writeLegacyUsersFile(const vector<tuple<String, String, bool>>& accounts)
{
    const auto path = filesystem::temp_directory_path() /
                      ("xmq_users_" + to_string(::getpid()) + ".conf");

    String json = R"({"users":[)";
    auto   id = 1;
    for (const auto& [username, password, admin]: accounts)
    {
        if (id > 1)
        {
            json += ",";
        }
        json += R"({"id":)" + to_string(id) + R"(,"username":")" + username + R"(","password":")" +
                UserManager::makeUserToken(username.c_str(), password.c_str(), "", "", DateTime()) +
                R"(","is_enabled":true,"is_admin":)" + (admin ? "true" : "false") + "}";
        ++id;
    }
    json += "]}";

    Buffer(json).saveToFile(path);
    return path;
}

} // namespace

// The first thing a broker does with a database: create it empty, and bring across whatever
// accounts the old file holds. The file is where the passwords are still readable, so this is the
// one moment a real verifier can be computed for an existing account without asking its owner for
// a new password.
TEST_F(XMQ_UserStoreTests, importsTheAccountsOfAnOldUsersFile)
{
    const auto usersFile = writeLegacyUsersFile({{"admin", "admin-secret", true},
                                                 {"cluster", "cluster-secret", false}});

    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);
        ASSERT_TRUE(store.users().empty()) << name << ": the store should start empty";

        EXPECT_EQ(2U, store.importUsersFrom(usersFile, 1000)) << name;

        const auto admin = store.findUser("admin");
        ASSERT_TRUE(admin) << name << ": admin was not imported";
        // Being an administrator is a membership now, and the migration puts the accounts that
        // were administrators into the group.
        const auto adminGroups = store.groupsOfUser(admin->m_id);
        EXPECT_EQ(2U, adminGroups.size()) << name << ": admin should be in Default and Administrators";
        EXPECT_TRUE(admin->m_enabled) << name;

        // The password came across, and what is stored is no longer the password.
        EXPECT_TRUE(PasswordHash::verify("admin-secret", admin->m_password))
            << name << ": the migrated password does not verify";
        EXPECT_TRUE(PasswordHash::isHashed(admin->m_password))
            << name << ": something other than a verifier was stored";
        EXPECT_EQ(String::npos, admin->m_password.find("admin-secret"))
            << name << ": the password itself survived into the database";

        const auto cluster = store.findUser("cluster");
        ASSERT_TRUE(cluster) << name;
        const auto clusterGroups = store.groupsOfUser(cluster->m_id);
        ASSERT_EQ(1U, clusterGroups.size()) << name << ": an ordinary account joined more than Default";
        EXPECT_EQ("Default", clusterGroups[0].m_name)
            << name << ": an ordinary account gained administrator rights";

        // Run twice, as a broker that restarts does. Nothing is added and nothing is overwritten:
        // the file stops being the authority the moment its accounts are in the database.
        EXPECT_EQ(0U, store.importUsersFrom(usersFile, 1000)) << name << ": imported twice";
        EXPECT_EQ(2U, store.users().size()) << name;
    }

    filesystem::remove(usersFile);
}

// No file is the ordinary case - a fresh installation - and not something to fail on.
TEST_F(XMQ_UserStoreTests, aMissingUsersFileIsNotAnError)
{
    auto store = storeOn(m_databases.front().second);

    EXPECT_EQ(0U, store.importUsersFrom("/nonexistent/xmq_users.conf", 1000));
    EXPECT_TRUE(store.users().empty());
}

// Membership is read from both ends - everything one account belongs to, and everyone in one
// group - and an account belongs to as many groups as it is put in. One group per account was the
// alternative considered and rejected: LDAP puts people in several, and LDAP is why authentication
// is becoming an extension.
TEST_F(XMQ_UserStoreTests, anAccountBelongsToEveryGroupItIsPutIn)
{
    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);

        const auto alice = store.addUser({.m_username = "alice", .m_password = "x"});
        const auto bob = store.addUser({.m_username = "bob", .m_password = "x"});
        const auto operators = store.addGroup({.m_name = "operators"});
        const auto auditors = store.addGroup({.m_name = "auditors"});

        store.addUserToGroup(alice, operators);
        store.addUserToGroup(alice, auditors);
        store.addUserToGroup(bob, operators);

        const auto aliceGroups = store.groupsOfUser(alice);
        ASSERT_EQ(2U, aliceGroups.size()) << name;
        EXPECT_EQ("auditors", aliceGroups[0].m_name) << name << ": groups come back by name";
        EXPECT_EQ("operators", aliceGroups[1].m_name) << name;

        EXPECT_EQ(1U, store.groupsOfUser(bob).size()) << name;
        EXPECT_EQ(2U, store.usersInGroup(operators).size()) << name;
        EXPECT_EQ(1U, store.usersInGroup(auditors).size()) << name;

        // Saying it twice changes nothing. An interface that sends an account's whole membership
        // on every save would otherwise fail on the rows that had not changed.
        store.addUserToGroup(alice, operators);
        EXPECT_EQ(2U, store.groupsOfUser(alice).size()) << name << ": a membership was duplicated";

        store.removeUserFromGroup(alice, operators);
        EXPECT_EQ(1U, store.groupsOfUser(alice).size()) << name;
        store.removeUserFromGroup(alice, operators);
        EXPECT_EQ(1U, store.groupsOfUser(alice).size()) << name << ": removing twice is not an error";
    }
}

// The cascades the schema declares, exercised rather than trusted. A membership row that outlives
// what it points at would be read as somebody being in a group that no longer exists, and
// permissions are not a place for rows nobody can account for.
TEST_F(XMQ_UserStoreTests, deletingAnAccountOrAGroupTakesItsMembershipsWithIt)
{
    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);

        const auto user = store.addUser({.m_username = "temporary", .m_password = "x"});
        const auto keptUser = store.addUser({.m_username = "kept", .m_password = "x"});
        const auto group = store.addGroup({.m_name = "temporary-group"});
        const auto keptGroup = store.addGroup({.m_name = "kept-group"});

        store.addUserToGroup(user, group);
        store.addUserToGroup(user, keptGroup);
        store.addUserToGroup(keptUser, group);

        store.removeUser(user);
        EXPECT_FALSE(store.findUser("temporary")) << name;
        EXPECT_EQ(1U, store.usersInGroup(group).size())
            << name << ": a membership survived the account it belonged to";
        EXPECT_EQ(0U, store.usersInGroup(keptGroup).size()) << name;

        store.removeGroup(group);
        EXPECT_FALSE(store.findGroup("temporary-group")) << name;
        EXPECT_TRUE(store.findUser("kept")) << name << ": deleting a group deleted an account";
        EXPECT_EQ(0U, store.groupsOfUser(keptUser).size())
            << name << ": a membership survived the group it was in";
    }
}

// Both names are unique, and the database is what says so - not code that looks first and is then
// overtaken by another node doing the same.
TEST_F(XMQ_UserStoreTests, namesAreUnique)
{
    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);

        store.addUser({.m_username = "only-one", .m_password = "x"});
        EXPECT_THROW(store.addUser({.m_username = "only-one", .m_password = "y"}), Exception)
            << name << ": a second account took an existing username";

        store.addGroup({.m_name = "only-group"});
        EXPECT_THROW(store.addGroup({.m_name = "only-group"}), Exception)
            << name << ": a second group took an existing name";
    }
}

// The one secret that has to survive the migration readable. Every node presents the 'cluster'
// account's password to its peers, so it moves into the configuration, while the account itself
// keeps a verifier like every other - presenting a secret and checking one being different needs.
TEST_F(XMQ_UserStoreTests, theClusterPasswordCanStillBeReadWhileTheFileExists)
{
    const auto usersFile = writeLegacyUsersFile({{"admin", "admin-secret", true},
                                                 {"cluster", "shared-between-nodes", false}});

    const auto password = UserStore::legacyPasswordFrom(usersFile, "cluster");
    ASSERT_TRUE(password) << "the cluster secret could not be read out of the old file";
    EXPECT_EQ("shared-between-nodes", *password);

    EXPECT_FALSE(UserStore::legacyPasswordFrom(usersFile, "nobody"))
        << "an account that is not there must not answer with somebody else's password";
    EXPECT_FALSE(UserStore::legacyPasswordFrom("/nonexistent/xmq_users.conf", "cluster"));

    // And after the migration it is a verifier like the rest: readable no longer.
    auto store = storeOn(m_databases.front().second);
    store.importUsersFrom(usersFile, 1000);

    const auto cluster = store.findUser("cluster");
    ASSERT_TRUE(cluster);
    EXPECT_TRUE(PasswordHash::isHashed(cluster->m_password));
    EXPECT_EQ(String::npos, cluster->m_password.find("shared-between-nodes"));
    EXPECT_TRUE(PasswordHash::verify("shared-between-nodes", cluster->m_password))
        << "peers could no longer be checked against it";

    filesystem::remove(usersFile);
}

// The first-start administrator has no password at all, and that has to survive being written and
// read back - it is the state the setup door depends on.
TEST_F(XMQ_UserStoreTests, anAccountWithNoPasswordSurvivesTheRoundTrip)
{
    for (const auto& [name, uri]: m_databases)
    {
        auto store = storeOn(uri);
        store.addUser({.m_username = "no-password-yet", .m_password = ""});

        const auto read = store.findUser("no-password-yet");
        ASSERT_TRUE(read) << name;
        EXPECT_TRUE(read->m_password.empty()) << name << ": an empty password came back as something";

        const auto all = store.users();
        ASSERT_EQ(1U, all.size()) << name;
        EXPECT_TRUE(all[0].m_password.empty()) << name;

        // And beside an account that does have one, which is how a real installation looks: the
        // administrator with no password yet, and the cluster account with one.
        store.addUser({.m_username = "with-password",
                       .m_password = PasswordHash::hash("something", 1000)});

        const auto both = store.users();
        ASSERT_EQ(2U, both.size()) << name;
        EXPECT_TRUE(both[0].m_password.empty()) << name << ": rows came back mixed up";
        EXPECT_FALSE(both[1].m_password.empty()) << name;
    }
}
