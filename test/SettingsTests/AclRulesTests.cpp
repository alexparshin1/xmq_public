#include "common/DirectoryNames.h"
#include "server/Settings/AclRules.h"
#include "server/Settings/UserManager.h"
#include "server/Settings/UserStore.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

class AclRulesTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto file = filesystem::temp_directory_path() /
                          ("xmq_acl_rules_" + to_string(::getpid()) + ".db");
        filesystem::remove(file);
        m_pool = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
        UserStore store(m_pool);
        store.createSchema();
        store.addAclRule({.m_groupName = "user:alice",
                          .m_action = "publish",
                          .m_deny = false,
                          .m_filter = "sensors/#"});
        store.addAclRule({.m_groupName = "user:alice",
                          .m_action = "publish",
                          .m_deny = true,
                          .m_filter = "sensors/secret/#"});
        store.addAclRule({.m_groupName = "operators",
                          .m_action = "subscribe",
                          .m_deny = false,
                          .m_filter = "plant/#"});
        m_rules.reload(m_pool, m_users);
    }

    shared_ptr<DatabaseConnectionPool> m_pool;
    UserManager                         m_users {[] {}};
    AclRules                            m_rules;
};

} // namespace

TEST_F(AclRulesTests, aGroupWithRulesIsAnAllowListAndDenyWins)
{
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("alice", "sensors/temp", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "sensors/secret/key", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "other/topic", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "sensors/temp", false))
        << "a publish rule does not grant subscribe";
}

TEST_F(AclRulesTests, aUserWithNoGroupRulesIsNotHandled)
{
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("bob", "sensors/temp", true));
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("", "anything", true));
}

TEST_F(AclRulesTests, accountGroupsAreEvaluatedByTheirName)
{
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorizeGroups({"operators"}, "plant/line1", false));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorizeGroups({"operators"}, "other/line1", false));
}

TEST_F(AclRulesTests, reloadingRulesDropsTheCachedGroups)
{
    UserStore store(m_pool);
    store.addAclRule({.m_groupName = "user:bob",
                      .m_action = "publish",
                      .m_deny = false,
                      .m_filter = "devices/#"});
    m_rules.reload(m_pool, m_users);

    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("bob", "devices/1", true));
    m_rules.invalidateGroups();
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("bob", "devices/1", true));
}

TEST_F(AclRulesTests, emptyWhenThereAreNoRules)
{
    const auto file = filesystem::temp_directory_path() /
                      ("xmq_acl_empty_" + to_string(::getpid()) + ".db");
    filesystem::remove(file);
    auto empty = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
    AclRules rules;
    rules.reload(empty, m_users);
    EXPECT_TRUE(rules.empty());
    EXPECT_EQ(AclRules::Decision::NotHandled, rules.authorize("alice", "sensors/temp", true));
}
