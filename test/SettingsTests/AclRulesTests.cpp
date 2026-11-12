#include "common/DirectoryNames.h"
#include "server/Settings/AclRules.h"
#include "server/Settings/UserManager.h"
#include "server/Settings/UserStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>

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
        store.addGroup({.m_name = "operators"});
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
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("alice", "client-1", "sensors/temp", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "client-1", "sensors/secret/key", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "client-1", "other/topic", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "client-1", "sensors/temp", false))
        << "a publish rule does not grant subscribe";
}

TEST_F(AclRulesTests, aUserWithNoGroupRulesIsNotHandled)
{
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("bob", "client-1", "sensors/temp", true));
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("", "", "anything", true));
}

TEST_F(AclRulesTests, externalIdentityRequiresAnExplicitNativeAllow)
{
    EXPECT_EQ(AclRules::Decision::Allow,
              m_rules.authorizeIdentity({"operators"}, "ldap/alice", "client", "plant/line1", false));
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorizeIdentity({"operators"}, "ldap/alice", "client", "plant/line1", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorizeIdentity({"without-rules"}, "ldap/alice", "client", "anything", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorizeIdentity({}, "ldap/alice", "client", "anything", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorizeIdentity({"operators"}, "alice", "client", "sensors/temp", true))
        << "A local account with the same name must not supply its own groups";
}

TEST_F(AclRulesTests, externalMembershipLosesAccessWhenTheMappedGroupIsDeleted)
{
    UserStore store(m_pool);
    const auto groups = store.groups();
    const auto found = ranges::find_if(groups, [](const auto& group) { return group.m_name == "operators"; });
    ASSERT_NE(found, groups.end());
    store.removeGroup(found->m_id);
    m_rules.reload(m_pool, m_users);
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorizeIdentity({"operators"}, "ldap/alice", "client", "plant/line1", false));
}

TEST_F(AclRulesTests, accountGroupsAreEvaluatedByTheirName)
{
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorizeGroups({"operators"}, "", "", "plant/line1", false));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorizeGroups({"operators"}, "", "", "other/line1", false));
}

TEST_F(AclRulesTests, reloadingRulesDropsTheCachedGroups)
{
    UserStore store(m_pool);
    store.addAclRule({.m_groupName = "user:bob",
                      .m_action = "publish",
                      .m_deny = false,
                      .m_filter = "devices/#"});
    m_rules.reload(m_pool, m_users);

    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("bob", "client-1", "devices/1", true));
    m_rules.invalidateGroups();
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("bob", "client-1", "devices/1", true));
}

TEST_F(AclRulesTests, emptyWhenThereAreNoRules)
{
    const auto file = filesystem::temp_directory_path() /
                      ("xmq_acl_empty_" + to_string(::getpid()) + ".db");
    filesystem::remove(file);
    auto empty = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
    // The schema is the settings'/authenticator's work; reload only reads it. A server that has
    // just created its accounts database reads no rules until one is added, which is this state.
    UserStore(empty).createSchema();
    AclRules rules;
    rules.reload(empty, m_users);
    EXPECT_TRUE(rules.empty());
    EXPECT_EQ(AclRules::Decision::NotHandled, rules.authorize("alice", "client-1", "sensors/temp", true));
}

TEST_F(AclRulesTests, aPatternRuleSubstitutesTheClientId)
{
    UserStore store(m_pool);
    store.addAclRule({.m_groupName = "*",
                      .m_action = "subscribe",
                      .m_deny = false,
                      .m_filter = "devices/%c/#",
                      .m_source = "mosquitto",
                      .m_order = 100,
                      .m_pattern = true});
    m_rules.reload(m_pool, m_users);

    EXPECT_EQ(AclRules::Decision::Allow,
              m_rules.authorize("bob", "device-7", "devices/device-7/temp", false));
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorize("bob", "device-8", "devices/device-7/temp", false))
        << "another client id does not match";
    EXPECT_EQ(AclRules::Decision::Deny,
              m_rules.authorize("bob", "+", "devices/+/temp", false))
        << "a client id that would add a wildcard matches nothing";
}

TEST_F(AclRulesTests, theEarliestOrderWinsEvenBetweenAllowAndDeny)
{
    UserStore store(m_pool);
    store.addAclRule({.m_groupName = "user:carol",
                      .m_action = "publish",
                      .m_deny = true,
                      .m_filter = "sensors/#",
                      .m_source = "dynamic-security",
                      .m_order = 10});
    store.addAclRule({.m_groupName = "user:carol",
                      .m_action = "publish",
                      .m_deny = false,
                      .m_filter = "sensors/temp",
                      .m_source = "dynamic-security",
                      .m_order = 20});
    m_rules.reload(m_pool, m_users);

    // The first rule in Mosquitto's order decides, even when a later one says the opposite.
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("carol", "client", "sensors/temp", true));
}

TEST_F(AclRulesTests, anonymousRulesDoNotApplyToNamedClients)
{
    UserStore store(m_pool);
    store.addAclRule({.m_groupName = "anonymous",
                      .m_action = "publish",
                      .m_deny = false,
                      .m_filter = "public/#",
                      .m_order = 100});
    m_rules.reload(m_pool, m_users);

    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("bob", "client", "public/x", true));
    EXPECT_EQ(AclRules::Decision::Allow, m_rules.authorize("", "", "public/x", true));
}

TEST_F(AclRulesTests, noMatchingRuleDeniesAClientTheRulesKnow)
{
    // alice has publish rules; a topic none of them covers is denied rather than left open.
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("alice", "client", "elsewhere", true));
    // bob has no rules at all, so the decision belongs to the extensions, as it always did.
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("bob", "client", "elsewhere", true));
}

TEST_F(AclRulesTests, defaultDenyDeniesAClientTheRulesDoNotKnow)
{
    UserStore store(m_pool);
    store.setDefaultDeny(true);
    m_rules.reload(m_pool, m_users);

    // bob has no rules at all; before this setting the decision belonged to the extensions.
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("bob", "client", "anything", true));
    EXPECT_EQ(AclRules::Decision::Deny, m_rules.authorize("", "", "anything", false));

    store.setDefaultDeny(false);
    m_rules.reload(m_pool, m_users);
    EXPECT_EQ(AclRules::Decision::NotHandled, m_rules.authorize("bob", "client", "anything", true));
}

TEST_F(AclRulesTests, defaultDenyAppliesEvenWithNoRulesAtAll)
{
    const auto file = filesystem::temp_directory_path() /
                      ("xmq_acl_empty_deny_" + to_string(::getpid()) + ".db");
    filesystem::remove(file);
    auto empty = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
    {
        UserStore store(empty);
        store.createSchema();
        store.setDefaultDeny(true);
    }
    AclRules rules;
    rules.reload(empty, m_users);
    EXPECT_EQ(AclRules::Decision::Deny, rules.authorize("anyone", "client", "anything", true));
}
