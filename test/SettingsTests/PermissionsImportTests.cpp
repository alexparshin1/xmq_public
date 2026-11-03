#include "server/Settings/PermissionsImport.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

bool hasRule(const vector<PermissionsImport::Rule>& rules, const string& group, const string& action,
             const bool deny, const string& filter)
{
    return ranges::any_of(rules, [&](const auto& rule)
                          {
                              return rule.m_group == group && rule.m_action == action && rule.m_deny == deny &&
                                     rule.m_filter == filter;
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
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "subscribe", false, "devices/sensor-1/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "publish", true, "site/sensor-1/telemetry/secret/#"));
    EXPECT_TRUE(hasRule(rules, "user:admin", "subscribe", false, "#"));
    EXPECT_TRUE(ranges::any_of(skipped, [](const auto& note) { return note.find("%c") != string::npos; }));
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
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "publish", false, "site/sensor-1/telemetry/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "subscribe", false, "site/sensor-1/config/#"));
    EXPECT_TRUE(hasRule(rules, "user:sensor-1", "subscribe", false, "plant/#"));
    EXPECT_TRUE(hasRule(rules, "user:admin", "publish", false, "#"));
    EXPECT_TRUE(ranges::any_of(skipped, [](const auto& note) { return note.find("deny-wins") != string::npos; }));
}
