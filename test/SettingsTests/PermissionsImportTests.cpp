#include "server/Settings/PermissionsImport.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

bool hasRule(const vector<PermissionsImport::Rule>& rules, const string& group, const string& action,
             const bool deny, const string& filter, const bool pattern = false)
{
    return ranges::any_of(rules, [&](const auto& rule)
                          {
                              return rule.m_group == group && rule.m_action == action && rule.m_deny == deny &&
                                     rule.m_filter == filter && rule.m_pattern == pattern;
                          });
}

} // namespace

TEST(PermissionsImport, readsAMosquittoAclFile)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(R"(
        topic read $SYS/#
        user sensor-1
        topic write site/sensor-1/telemetry/#
        topic read site/sensor-1/config/#
        topic deny site/sensor-1/telemetry/secret/#
        pattern read devices/%u/#
        pattern read devices/%c/#
        user admin
        topic readwrite #
    )", skipped);

    EXPECT_TRUE(hasRule(rules, "anonymous", "subscribe", false, "$SYS/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "publish", false, "site/sensor-1/telemetry/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "publish", true, "site/sensor-1/telemetry/secret/#"));
    EXPECT_TRUE(hasRule(rules, "user:admin", "subscribe", false, "#"));

    // A pattern is one rule for every client, with its tokens intact: %c can be honoured now, and
    // %u does not depend on the accounts that happen to exist at import time.
    EXPECT_TRUE(hasRule(rules, "*", "subscribe", false, "devices/%u/#", true));
    EXPECT_TRUE(hasRule(rules, "*", "subscribe", false, "devices/%c/#", true));
    EXPECT_FALSE(ranges::any_of(skipped, [](const auto& note) { return note.find("%c") != string::npos; }));
}

TEST(PermissionsImport, duplicateAndConflictClassification)
{
    const vector<UserStore::AclRule> existing {
        {.m_groupName = "user:u", .m_action = "publish", .m_deny = false, .m_filter = "sensors/#"},
    };
    const vector<PermissionsImport::Rule> imported {
        {"user:u", "publish", false, "sensors/#", "mosquitto", "acl:1"},
        {"user:u", "publish", true, "sensors/secret/#", "mosquitto", "acl:2"},
        {"user:u", "publish", false, "sensors/temp", "mosquitto", "acl:3"},
    };
    const auto plan = PermissionsImport::plan(existing, imported);
    EXPECT_EQ(1U, plan.m_unchanged.size());
    EXPECT_EQ(1U, plan.m_redundant.size());
    ASSERT_EQ(1U, plan.m_conflicts.size());
    EXPECT_EQ("sensors/secret/#", plan.m_conflicts.front().m_scope);
}

TEST(PermissionsImport, applyReplacesOrSkipsTheConflictingRule)
{
    const vector<UserStore::AclRule> existing {
        {.m_groupName = "user:u", .m_action = "publish", .m_deny = false, .m_filter = "sensors/#"},
    };
    const vector<PermissionsImport::Rule> imported {
        {"user:u", "publish", true, "sensors/secret/#", "mosquitto", "acl:1"},
    };
    const auto plan = PermissionsImport::plan(existing, imported);

    const auto kept = PermissionsImport::apply(existing, plan, "existing");
    EXPECT_EQ(1U, kept.size()) << "skipping the conflict leaves the existing rule alone";

    const auto replaced = PermissionsImport::apply(existing, plan, "imported");
    ASSERT_EQ(1U, replaced.size());
    EXPECT_TRUE(replaced.front().m_deny);
    EXPECT_EQ("sensors/secret/#", replaced.front().m_filter);
}

TEST(PermissionsImport, readsDynamicSecurityRolesAndDefaults)
{
    const auto content = R"({
        "clients": [
            {"username": "sensor-1", "roles": [{"rolename": "sensor"}],
             "groups": [{"groupname": "plant"}]},
            {"username": "admin", "roles": [{"rolename": "admin"}]}
        ],
        "groups": [{"groupname": "plant", "roles": [{"rolename": "plant-read"}]}],
        "roles": [
            {"rolename": "sensor", "acls": [
                {"acltype": "publishClientSend", "topic": "site/%u/telemetry/#", "allow": true},
                {"acltype": "subscribePattern", "topic": "site/%u/config/#", "allow": true}
            ]},
            {"rolename": "plant-read", "acls": [
                {"acltype": "subscribePattern", "topic": "plant/#", "allow": true}
            ]},
            {"rolename": "admin", "acls": [
                {"acltype": "publishClientSend", "topic": "#", "allow": true},
                {"acltype": "subscribePattern", "topic": "#", "allow": true}
            ]}
        ],
        "defaultACLAccess": {"publishClientSend": false, "publishClientReceive": true,
                             "subscribe": false, "unsubscribe": true}
    })";

    vector<string> skipped;
    const auto rules = PermissionsImport::parseDynamicSecurity(content, skipped);
    // %u stays in the filter: it is replaced when the rule is checked, as in Mosquitto.
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "publish", false, "site/%u/telemetry/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "subscribe", false, "site/%u/config/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "subscribe", false, "plant/#"));
    EXPECT_TRUE(hasRule(rules, "user:admin", "publish", false, "#"));

    // The direct role is checked before the role that comes through the group.
    const auto direct = ranges::find_if(rules, [](const auto& rule)
                                        { return rule.m_filter == "site/%u/telemetry/#"; });
    const auto grouped = ranges::find_if(rules, [](const auto& rule)
                                         { return rule.m_filter == "plant/#"; });
    ASSERT_NE(rules.end(), direct);
    ASSERT_NE(rules.end(), grouped);
    EXPECT_LT(direct->m_order, grouped->m_order);
    EXPECT_TRUE(ranges::any_of(skipped, [](const auto& note)
                              { return note.find("substituted when the rule is checked") != string::npos; }));
}

TEST(PermissionsImport, refusesAFilterWithAnEmptyLevel)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(R"(
        user u
        topic read //s/#
        topic read s/+/temp
    )", skipped);

    // The same rule the control API applies, so an import cannot store what a manual add refuses.
    EXPECT_TRUE(hasRule(rules, "user:u", "subscribe", false, "s/+/temp"));
    EXPECT_FALSE(hasRule(rules, "user:u", "subscribe", false, "//s/#"));
    EXPECT_TRUE(ranges::any_of(skipped, [](const auto& note)
                              { return note.find("//s/#") != string::npos; }));
}
