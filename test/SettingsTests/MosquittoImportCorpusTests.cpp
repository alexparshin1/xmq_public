#include "common/DirectoryNames.h"
#include "server/Settings/AclRules.h"
#include "server/Settings/PasswordImport.h"
#include "server/Settings/PermissionsImport.h"
#include "server/Settings/UserStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;
namespace ranges = std::ranges;

namespace {

#ifndef XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY
#error "XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY is not defined"
#endif

const filesystem::path corpusDirectory {XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY};

/// This process, so two runs at once do not share a temporary database.
int processId()
{
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

string readFile(const filesystem::path& path)
{
    ifstream stream(path);
    if (!stream)
    {
        throw runtime_error("Cannot read " + path.string());
    }
    stringstream content;
    content << stream.rdbuf();
    return content.str();
}

const PermissionsImport::Rule* findRule(const vector<PermissionsImport::Rule>& rules,
                                        const string& group, const string& action,
                                        const string& filter, const bool pattern = false)
{
    const auto found = ranges::find_if(rules, [&](const auto& rule)
                                      {
                                          return rule.m_group == group && rule.m_action == action &&
                                                 rule.m_filter == filter && rule.m_pattern == pattern;
                                      });
    return found == rules.end() ? nullptr : &*found;
}

vector<const PermissionsImport::Rule*> ordered(const vector<PermissionsImport::Rule>& rules,
                                               const string& group)
{
    vector<const PermissionsImport::Rule*> result;
    for (const auto& rule: rules)
    {
        if (rule.m_group == group)
        {
            result.push_back(&rule);
        }
    }
    ranges::sort(result, [](const auto* first, const auto* second)
                 { return first->m_order < second->m_order; });
    return result;
}

AclRules::Decision decide(const vector<PermissionsImport::Rule>& rules,
                          const vector<string>& groups, const string& username,
                          const string& clientId, const string& topic, const bool publish)
{
    vector<UserStore::AclRule> stored;
    for (const auto& rule: rules)
    {
        stored.push_back({.m_groupName = rule.m_group.c_str(),
                          .m_action = rule.m_action.c_str(),
                          .m_deny = rule.m_deny,
                          .m_filter = rule.m_filter.c_str(),
                          .m_source = rule.m_source.c_str(),
                          .m_sourceRef = rule.m_ref.c_str(),
                          .m_order = rule.m_order,
                          .m_pattern = rule.m_pattern});
    }
    return AclRules::evaluate(stored, groups, username, clientId, topic, publish);
}

} // namespace

/**
 * The corpus is a copy of rules that Mosquitto itself ships or generates in its test suite:
 * aclfile.example, the ACL strings in test/broker/09-*.py, and the Dynamic Security configurations.
 * Every file in the directory is parsed here, so a file added to the corpus cannot be overlooked.
 */
TEST(MosquittoImportCorpus, everyFileIsImported)
{
    const set<string> expected {
        "acl-access-variants.acl", "acl-change.acl", "acl-empty.acl", "aclfile-example.acl",
        "acl-mixed-patterns.acl", "acl-persist-deny.acl", "dynamic-security-init.json",
        "dynamic-security-priorities.json",
        "password-file-mixed.pwfile", "password-file-pbkdf2.pwfile",
        "password-file-plain.pwfile", "password-file-sha512-crypt.pwfile"};

    set<string> seen;
    for (const auto& entry: filesystem::directory_iterator(corpusDirectory))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name == "README.md")
        {
            continue;
        }
        seen.insert(name);
        const auto content = readFile(entry.path());
        const auto extension = entry.path().extension().string();
        ASSERT_NO_THROW({
            vector<string> skipped;
            if (extension == ".acl")
            {
                PermissionsImport::parseAcl(content, skipped);
            }
            else if (extension == ".json")
            {
                PermissionsImport::parseDynamicSecurity(content, skipped);
            }
            else if (extension == ".pwfile")
            {
                PasswordImport::parse(content);
            }
            else
            {
                FAIL() << "The corpus contains a file of an unknown kind: " << name;
            }
        }) << name;
    }

    for (const auto& name: expected)
    {
        EXPECT_TRUE(seen.contains(name)) << "missing corpus file " << name;
    }
    EXPECT_EQ(expected.size(), seen.size()) << "a corpus file was added without a check here";
}

TEST(MosquittoImportCorpus, accessVariantsKeepGlobalUserAndPatternApart)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(
        readFile(corpusDirectory / "acl-access-variants.acl"), skipped);

    ASSERT_NE(nullptr, findRule(rules, "anonymous", "subscribe", "topic/global/#"));
    ASSERT_NE(nullptr, findRule(rules, "user:username", "publish", "topic/username/#"));
    ASSERT_NE(nullptr, findRule(rules, "*", "publish", "pattern/%u/#", true));

    // Mosquitto handles every deny before any allow.
    int lastDeny = -1;
    int firstAllow = INT_MAX;
    for (const auto& rule: rules)
    {
        if (rule.m_deny)
        {
            lastDeny = max(lastDeny, rule.m_order);
        }
        else
        {
            firstAllow = min(firstAllow, rule.m_order);
        }
    }
    EXPECT_LT(lastDeny, firstAllow);

    // The global rules belong to anonymous clients only: a named client with its own rules does not
    // inherit them.
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"anonymous"}, "", "", "topic/global", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:username"}, "username", "acl-check", "topic/global", true));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:username"}, "username", "acl-check", "topic/username", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:username"}, "username", "acl-check", "topic/username/except", true));

    // The pattern is one rule for every client, with the username substituted when checked.
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:username"}, "username", "acl-check", "pattern/username/x", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:username"}, "username", "acl-check", "pattern/username/except", true));
}

TEST(MosquittoImportCorpus, aclfileExampleImportsTheClientIdPattern)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(
        readFile(corpusDirectory / "aclfile-example.acl"), skipped);

    ASSERT_NE(nullptr, findRule(rules, "anonymous", "subscribe", "$SYS/#"));
    ASSERT_NE(nullptr, findRule(rules, "user:roger", "publish", "foo/bar"));
    ASSERT_NE(nullptr, findRule(rules, "*", "publish", "$SYS/broker/connection/%c/state", true));

    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:roger"}, "roger", "gw-1",
                     "$SYS/broker/connection/gw-1/state", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:roger"}, "roger", "gw-2",
                     "$SYS/broker/connection/gw-1/state", true))
        << "another client id does not match the pattern";
}

TEST(MosquittoImportCorpus, aclChangeKeepsTheFileOrder)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(readFile(corpusDirectory / "acl-change.acl"), skipped);
    const auto byOrder = ordered(rules, "user:username");
    // Each readwrite line is a publish rule and a subscribe rule; the first line is checked first.
    ASSERT_EQ(4U, byOrder.size());
    EXPECT_EQ("topic/one", byOrder[0]->m_filter);
    EXPECT_EQ("topic/one", byOrder[1]->m_filter);
    EXPECT_EQ("topic/two", byOrder[2]->m_filter);
    EXPECT_EQ("topic/two", byOrder[3]->m_filter);
}

TEST(MosquittoImportCorpus, theEmptyFileImportsNothing)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(readFile(corpusDirectory / "acl-empty.acl"), skipped);
    EXPECT_TRUE(rules.empty());
    EXPECT_EQ(AclRules::Decision::NotHandled,
              decide(rules, {"anonymous"}, "", "", "test/topic", true));
}

TEST(MosquittoImportCorpus, patternDenyWinsOverPatternAllow)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(
        readFile(corpusDirectory / "acl-persist-deny.acl"), skipped);

    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:test-acl-change"}, "test-acl-change", "c", "client-msg/test", true));
    // A pattern applies to every client, so the deny reaches clients the file never names.
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:someone-else"}, "someone-else", "c", "client-msg/test", false));
}

TEST(MosquittoImportCorpus, mixedPatternsCoverUserAndClientId)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseAcl(
        readFile(corpusDirectory / "acl-mixed-patterns.acl"), skipped);

    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"anonymous"}, "", "", "public/announcements/x", false));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:device"}, "device", "dev-1", "devices/device/commands/x", false));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:device"}, "device", "dev-1",
                     "devices/device/commands/secret/x", false));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:device"}, "device", "dev-1", "devices/dev-1/state", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:device"}, "device", "dev-1", "devices/dev-2/state", true));
}

TEST(MosquittoImportCorpus, dynamicSecurityInitCarriesRolesAndDefaults)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseDynamicSecurity(
        readFile(corpusDirectory / "dynamic-security-init.json"), skipped);

    ASSERT_NE(nullptr, findRule(rules, "user:admin", "publish", "$CONTROL/dynamic-security/#"));
    ASSERT_NE(nullptr, findRule(rules, "user:admin", "subscribe", "#"));
    EXPECT_TRUE(ranges::any_of(skipped, [](const auto& note)
                              { return note.find("publishClientReceive") != string::npos; }));

    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:admin"}, "admin", "c",
                     "$CONTROL/dynamic-security/v1", true));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, {"user:admin"}, "admin", "c", "some/topic", false));
    // The role grants no application publish; the subscribePattern '#' does not grant it either.
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, {"user:admin"}, "admin", "c", "some/topic", true));
}

TEST(MosquittoImportCorpus, dynamicSecurityPrioritiesProduceMosquittoOrder)
{
    vector<string> skipped;
    const auto rules = PermissionsImport::parseDynamicSecurity(
        readFile(corpusDirectory / "dynamic-security-priorities.json"), skipped);

    // Direct role, then the group with the higher client priority, then its roles by priority, then
    // the next group. Within a role, ACL priority decides: the deny on input/secret is checked
    // before the allow on input/%u.
    const auto byOrder = ordered(rules, "user:sensor");
    vector<string> filters;
    for (const auto* rule: byOrder)
    {
        filters.push_back(rule->m_filter);
    }
    EXPECT_EQ((vector<string> {"room/%c/temp", "room/%u/#", "input/secret/#", "input/%u/#",
                               "output/#", "humidity/%u/#"}), filters);

    const vector<string> groups {"user:sensor", "temperature", "humidity"};
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, groups, "sensor", "sensor-1", "room/sensor-1/temp", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, groups, "sensor", "sensor-1", "input/secret/x", true));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, groups, "sensor", "sensor-1", "input/sensor/data", true));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, groups, "sensor", "sensor-1", "output/x", true));
    EXPECT_EQ(AclRules::Decision::Allow,
              decide(rules, groups, "sensor", "sensor-1", "humidity/sensor/50", true));
    EXPECT_EQ(AclRules::Decision::Deny,
              decide(rules, groups, "sensor", "sensor-1", "elsewhere", true));
}

TEST(MosquittoImportCorpus, everyFileSurvivesPlanApplyAndTheStore)
{
    const auto file = filesystem::temp_directory_path() /
                      ("xmq_mosquitto_corpus_" + to_string(processId()) + ".db");
    filesystem::remove(file);
    const auto pool = make_shared<DatabaseConnectionPool>(DirectoryNames::sqliteUri(file), 2);
    UserStore  store(pool);
    store.createSchema();

    for (const auto& entry: filesystem::directory_iterator(corpusDirectory))
    {
        if (!entry.is_regular_file() || entry.path().filename() == "README.md")
        {
            continue;
        }
        if (entry.path().extension() == ".pwfile")
        {
            continue; // accounts are covered by PasswordImport's own tests
        }
        const auto content = readFile(entry.path());
        vector<string> skipped;
        vector<PermissionsImport::Rule> imported;
        if (entry.path().extension() == ".acl")
        {
            imported = PermissionsImport::parseAcl(content, skipped);
        }
        else
        {
            imported = PermissionsImport::parseDynamicSecurity(content, skipped);
        }

        const auto plan = PermissionsImport::plan({}, imported);
        const auto applied = PermissionsImport::apply({}, plan, "reject");
        store.replaceAclRules(applied);

        const auto stored = store.aclRules();
        EXPECT_EQ(applied.size(), stored.size()) << entry.path().filename();
        // The evaluation position and the pattern flag survive the round trip, not just the key.
        for (const auto& rule: stored)
        {
            const auto original = ranges::find_if(applied, [&](const auto& candidate)
                                                 {
                                                     return candidate.m_groupName == rule.m_groupName &&
                                                            candidate.m_action == rule.m_action &&
                                                            candidate.m_deny == rule.m_deny &&
                                                            candidate.m_filter == rule.m_filter &&
                                                            candidate.m_pattern == rule.m_pattern;
                                                 });
            ASSERT_NE(applied.end(), original) << entry.path().filename();
            EXPECT_EQ(original->m_order, rule.m_order) << entry.path().filename();
        }
    }
    filesystem::remove(file);
}
