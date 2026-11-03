#include "PermissionsImport.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>

#include <sptk5/xdoc/Document.h>

using namespace std;
using namespace sptk;
namespace ranges = std::ranges;

namespace xmq {

namespace {

/// Manual rules sit before anything an import brought in, and a manual deny before a manual allow.
constexpr int manualDenyOrder = 0;
constexpr int manualAllowOrder = 1000000;
/// Imported rules keep the order Mosquitto would check them in.
constexpr int importedOrderBase = 2000000;
/// A default ACL ("allow everything not otherwise matched") is checked last.
constexpr int defaultOrderBase = 4000000;

string trim(const string& text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

vector<string> levels(const string_view filter)
{
    vector<string> parts;
    size_t         start = 0;
    while (start <= filter.size())
    {
        const auto slash = filter.find('/', start);
        if (slash == string_view::npos)
        {
            parts.emplace_back(filter.substr(start));
            break;
        }
        parts.emplace_back(filter.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

bool validFilter(const string_view filter)
{
    if (filter.empty())
    {
        return false;
    }
    const auto parts = levels(filter);
    for (size_t index = 0; index < parts.size(); ++index)
    {
        // One rule for the control API, the import and the evaluator: no empty levels, and the
        // wildcards must each be a whole level, with # the last one. A %u or %c token is a level.
        if (parts[index].empty())
        {
            return false;
        }
        if (parts[index].find('#') != string::npos && (parts[index] != "#" || index + 1 != parts.size()))
        {
            return false;
        }
        if (parts[index].find('+') != string::npos && parts[index] != "+")
        {
            return false;
        }
    }
    return true;
}

string join(const vector<string>& parts, const string_view separator);

bool covers(const string_view broader, const string_view narrower)
{
    const auto a = levels(broader);
    const auto b = levels(narrower);
    if (!a.empty() && a.front() == "#")
    {
        return true;
    }
    for (size_t index = 0; index < min(a.size(), b.size()); ++index)
    {
        if (a[index] == "#")
        {
            return true;
        }
        if (b[index] == "#")
        {
            return false;
        }
        if (a[index] == "+")
        {
            continue;
        }
        if (b[index] == "+" || a[index] != b[index])
        {
            return false;
        }
    }
    if (a.size() == b.size())
    {
        return true;
    }
    if (a.size() < b.size())
    {
        return false;
    }
    return a[b.size()] == "#";
}

optional<string> intersection(const string_view first, const string_view second)
{
    if (covers(first, second))
    {
        return string(second);
    }
    if (covers(second, first))
    {
        return string(first);
    }

    const auto a = levels(first);
    const auto b = levels(second);
    vector<string> result;
    for (size_t index = 0; index < min(a.size(), b.size()); ++index)
    {
        const auto& left = a[index];
        const auto& right = b[index];
        if (left == "#")
        {
            result.insert(result.end(), b.begin() + index, b.end());
            return result.empty() ? nullopt : optional<string>(join(result, "/"));
        }
        if (right == "#")
        {
            result.insert(result.end(), a.begin() + index, a.end());
            return result.empty() ? nullopt : optional<string>(join(result, "/"));
        }
        if (left == "+")
        {
            result.push_back(right);
        }
        else if (right == "+")
        {
            result.push_back(left);
        }
        else if (left == right)
        {
            result.push_back(left);
        }
        else
        {
            return nullopt;
        }
    }
    if (a.size() > b.size())
    {
        if (a[b.size()] != "#")
        {
            return nullopt;
        }
        result.push_back("#");
    }
    else if (b.size() > a.size())
    {
        if (b[a.size()] != "#")
        {
            return nullopt;
        }
        result.push_back("#");
    }
    return join(result, "/");
}

string join(const vector<string>& parts, const string_view separator)
{
    string result;
    for (const auto& part: parts)
    {
        if (!result.empty())
        {
            result += separator;
        }
        result += part;
    }
    return result;
}

string jsonQuote(const string& text)
{
    string result = "\"";
    for (const auto character: text)
    {
        switch (character)
        {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20)
                {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                    result += buffer;
                }
                else
                {
                    result += character;
                }
        }
    }
    result += '"';
    return result;
}

string ruleJson(const PermissionsImport::Rule& rule)
{
    return "{\"group\":" + jsonQuote(rule.m_group) + ",\"action\":" + jsonQuote(rule.m_action) +
           ",\"deny\":" + (rule.m_deny ? "true" : "false") + ",\"filter\":" + jsonQuote(rule.m_filter) +
           ",\"source\":" + jsonQuote(rule.m_source) + ",\"ref\":" + jsonQuote(rule.m_ref) +
           ",\"order\":" + to_string(rule.m_order) +
           ",\"pattern\":" + (rule.m_pattern ? "true" : "false") + "}";
}

string ruleJson(const UserStore::AclRule& rule)
{
    return "{\"group\":" + jsonQuote(rule.m_groupName.c_str()) + ",\"action\":" + jsonQuote(rule.m_action.c_str()) +
           ",\"deny\":" + (rule.m_deny ? "true" : "false") + ",\"filter\":" + jsonQuote(rule.m_filter.c_str()) +
           ",\"source\":" + jsonQuote(rule.m_source.c_str()) + ",\"ref\":" + jsonQuote(rule.m_sourceRef.c_str()) +
           ",\"order\":" + to_string(rule.m_order) +
           ",\"pattern\":" + (rule.m_pattern ? "true" : "false") + "}";
}

string arrayJson(const vector<PermissionsImport::Rule>& rules)
{
    string result = "[";
    for (size_t index = 0; index < rules.size(); ++index)
    {
        if (index)
        {
            result += ",";
        }
        result += ruleJson(rules[index]);
    }
    return result + "]";
}

/// One ACL as Dynamic Security writes it, before the event type has been mapped onto an action.
struct DynAcl
{
    string m_type;
    string m_topic;
    bool   m_allow {true};
    int    m_priority {-1};
    size_t m_listed {0};
};

int nodePriority(const xdoc::SNode& node)
{
    const auto priority = node->findFirst("priority");
    return priority ? priority->getInteger() : -1;
}

/// A role's ACLs in Mosquitto's order: priority from highest to lowest, then as listed.
vector<DynAcl> roleAcls(const xdoc::SNode& role)
{
    vector<DynAcl> acls;
    size_t         listed = 0;
    for (const auto& acl: role->nodes("acls"))
    {
        acls.push_back({acl->getString("acltype").c_str(), acl->getString("topic").c_str(),
                        acl->getBoolean("allow"), nodePriority(acl), listed++});
    }
    ranges::stable_sort(acls, [](const auto& first, const auto& second)
                        {
                            if (first.m_priority != second.m_priority)
                            {
                                return first.m_priority > second.m_priority;
                            }
                            return first.m_listed < second.m_listed;
                        });
    return acls;
}

/// One role assignment, with the priority the client or group gives it.
struct RoleAssignment
{
    string m_name;
    int    m_priority {-1};
};

vector<RoleAssignment> roleAssignments(const xdoc::SNode& node)
{
    vector<RoleAssignment> assignments;
    for (const auto& assignment: node->nodes("roles"))
    {
        assignments.push_back({assignment->getString("rolename").c_str(), nodePriority(assignment)});
    }
    ranges::stable_sort(assignments, [](const auto& first, const auto& second)
                        {
                            if (first.m_priority != second.m_priority)
                            {
                                return first.m_priority > second.m_priority;
                            }
                            return first.m_name < second.m_name;
                        });
    return assignments;
}

struct GroupAssignment
{
    string m_name;
    int    m_priority {-1};
};

vector<GroupAssignment> groupAssignments(const xdoc::SNode& node)
{
    vector<GroupAssignment> groups;
    for (const auto& assignment: node->nodes("groups"))
    {
        groups.push_back({assignment->getString("groupname").c_str(), nodePriority(assignment)});
    }
    ranges::stable_sort(groups, [](const auto& first, const auto& second)
                        {
                            if (first.m_priority != second.m_priority)
                            {
                                return first.m_priority > second.m_priority;
                            }
                            return first.m_name < second.m_name;
                        });
    return groups;
}

} // namespace

vector<PermissionsImport::Rule> PermissionsImport::parseAcl(const string_view content,
                                                            vector<string>&  skipped)
{
    vector<Rule> rules;
    string       current = "anonymous";
    int          position = 0;

    size_t lineNumber = 0;
    istringstream stream{string(content)};
    string        raw;
    while (getline(stream, raw))
    {
        ++lineNumber;
        const auto text = trim(raw);
        if (text.empty() || text.front() == '#')
        {
            continue;
        }

        istringstream line(text);
        string        keyword;
        line >> keyword;
        if (keyword == "user")
        {
            string name;
            line >> name;
            current = name.empty() ? "anonymous" : "user:" + name;
            continue;
        }
        if (keyword != "topic" && keyword != "pattern")
        {
            skipped.push_back("line " + to_string(lineNumber) + ": unknown directive '" + keyword + "'");
            continue;
        }

        string access;
        string filter;
        line >> access;
        if (access == "read" || access == "write" || access == "readwrite" || access == "deny")
        {
            getline(line, filter);
        }
        else
        {
            filter = access;
            access = "readwrite";
        }
        filter = trim(filter);
        if (filter.empty() || !validFilter(filter))
        {
            skipped.push_back("line " + to_string(lineNumber) + ": invalid or empty topic '" + filter + "'");
            continue;
        }

        vector<pair<string, bool>> actions;
        if (access == "read" || access == "readwrite")
        {
            actions.emplace_back("subscribe", false);
        }
        if (access == "write" || access == "readwrite")
        {
            actions.emplace_back("publish", false);
        }
        if (access == "deny")
        {
            actions.emplace_back("subscribe", true);
            actions.emplace_back("publish", true);
        }

        // A pattern is a template for every client, so it is stored once with its %u/%c tokens
        // intact rather than expanded over the accounts that happen to exist today.
        const bool pattern = keyword == "pattern";
        for (const auto& [action, deny]: actions)
        {
            // Mosquitto checks every deny before any allow; otherwise the file order stands.
            const auto order = (deny ? importedOrderBase : importedOrderBase + 1000000) + position++;
            rules.push_back({.m_group = pattern ? "*" : current,
                             .m_action = action,
                             .m_deny = deny,
                             .m_filter = filter,
                             .m_source = "mosquitto",
                             .m_ref = "acl:" + to_string(lineNumber),
                             .m_order = order,
                             .m_pattern = pattern});
        }
    }
    return rules;
}

vector<PermissionsImport::Rule> PermissionsImport::parseDynamicSecurity(const string_view content,
                                                                        vector<string>&  skipped)
{
    xdoc::Document document;
    document.load(String(string(content)));
    const auto root = document.root();

    map<string, xdoc::SNode> roles;
    map<string, xdoc::SNode> groups;
    for (const auto& node: root->nodes("roles"))
    {
        roles[node->getString("rolename").c_str()] = node;
    }
    for (const auto& node: root->nodes("groups"))
    {
        groups[node->getString("groupname").c_str()] = node;
    }

    const auto defaults        = root->findFirst("defaultACLAccess");
    const bool defaultPublish   = defaults && defaults->getBoolean("publishClientSend");
    const bool defaultSubscribe = defaults && defaults->getBoolean("subscribe");
    const auto anonymousGroup = root->findFirst("anonymousGroup")
        ? root->getString("anonymousGroup") : String("unauthenticated");

    vector<Rule> rules;

    const auto addRule = [&rules](const string& group, const string& action, const bool deny,
                                  const string& filter, const string& ref, const int order)
    {
        rules.push_back({.m_group = group,
                         .m_action = action,
                         .m_deny = deny,
                         .m_filter = filter,
                         .m_source = "dynamic-security",
                         .m_ref = ref,
                         .m_order = order,
                         .m_pattern = false});
    };

    const auto addRole = [&](const string& rolename, const string& subject, const string& label,
                             int& position)
    {
        const auto found = roles.find(rolename);
        if (found == roles.end())
        {
            skipped.push_back(label + ": role '" + rolename + "' not found");
            return;
        }
        for (const auto& acl: roleAcls(found->second))
        {
            const auto ref = label + "/role/" + rolename;
            if (acl.m_topic.empty())
            {
                skipped.push_back(ref + ": empty topic");
                continue;
            }
            if (!validFilter(acl.m_topic))
            {
                skipped.push_back(ref + ": invalid filter '" + acl.m_topic + "'");
                continue;
            }
            const auto order = importedOrderBase + position++;
            if (acl.m_type == "publishClientSend")
            {
                addRule(subject, "publish", !acl.m_allow, acl.m_topic, ref, order);
            }
            else if (acl.m_type == "subscribeLiteral" || acl.m_type == "subscribePattern")
            {
                if (acl.m_type == "subscribeLiteral")
                {
                    skipped.push_back(ref + ": subscribeLiteral is approximated as a filter");
                }
                addRule(subject, "subscribe", !acl.m_allow, acl.m_topic, ref, order);
            }
            else if (acl.m_type == "publishClientReceive")
            {
                if (!acl.m_allow)
                {
                    skipped.push_back(ref + ": publishClientReceive deny is carried as a subscribe deny");
                    addRule(subject, "subscribe", true, acl.m_topic, ref, order);
                }
                else
                {
                    skipped.push_back(ref + ": publishClientReceive allow is implied by subscribe");
                }
            }
            else if (acl.m_type.starts_with("unsubscribe"))
            {
                skipped.push_back(ref + ": " + acl.m_type + " has no equivalent");
            }
            else
            {
                skipped.push_back(ref + ": unknown acltype '" + acl.m_type + "'");
            }
        }
    };

    for (const auto& client: root->nodes("clients"))
    {
        const string username = client->getString("username").c_str();
        if (username.empty())
        {
            skipped.push_back("client without a username is not imported");
            continue;
        }
        const string subject = "user:" + username;
        int          position = 0;

        // Direct roles first, in the order Mosquitto checks them.
        for (const auto& assignment: roleAssignments(client))
        {
            addRole(assignment.m_name, subject, "client/" + username + "/direct", position);
        }
        // Then the client's groups, by the priority the client gives each one.
        for (const auto& assignment: groupAssignments(client))
        {
            const auto found = groups.find(assignment.m_name);
            if (found == groups.end())
            {
                skipped.push_back("client/" + username + ": group '" + assignment.m_name + "' not found");
                continue;
            }
            for (const auto& role: roleAssignments(found->second))
            {
                addRole(role.m_name, subject,
                        "client/" + username + "/group/" + assignment.m_name, position);
            }
        }

        // Default access applies only when no ACL matched, so it is placed after everything else.
        if (defaultPublish)
        {
            addRule(subject, "publish", false, "#", "default/publishClientSend",
                    defaultOrderBase + position++);
        }
        if (defaultSubscribe)
        {
            addRule(subject, "subscribe", false, "#", "default/subscribe",
                    defaultOrderBase + position++);
        }
    }

    // The group Mosquitto uses for clients without a username.
    const auto anonymous = groups.find(anonymousGroup.c_str());
    if (anonymous != groups.end())
    {
        int position = 0;
        for (const auto& role: roleAssignments(anonymous->second))
        {
            addRole(role.m_name, "anonymous", "anonymous/group/" + string(anonymousGroup.c_str()),
                    position);
        }
        if (defaultPublish)
        {
            addRule("anonymous", "publish", false, "#", "default/publishClientSend",
                    defaultOrderBase + position++);
        }
        if (defaultSubscribe)
        {
            addRule("anonymous", "subscribe", false, "#", "default/subscribe",
                    defaultOrderBase + position++);
        }
    }

    skipped.push_back("ACL order follows Mosquitto's priorities; %u and %c are substituted when the "
                      "rule is checked, not when it is imported");
    return rules;
}

PermissionsImport::Plan PermissionsImport::plan(const vector<UserStore::AclRule>& existing,
                                                const vector<Rule>&               imported)
{
    Plan result;
    // Rules already met in this file. A file can contradict itself - an allow and a deny on the
    // same scope - and that is a conflict the operator has to answer just like one against a
    // stored rule. Mosquitto's own file order would decide it, but silently, which is not what an
    // import of somebody's access policy should do.
    vector<Rule> seen;

    const auto sameKey = [](const UserStore::AclRule& stored, const Rule& rule)
    {
        return stored.m_groupName == rule.m_group.c_str() && stored.m_action == rule.m_action.c_str() &&
               stored.m_deny == rule.m_deny && stored.m_filter == rule.m_filter.c_str() &&
               stored.m_pattern == rule.m_pattern;
    };

    for (const auto& rule: imported)
    {
        if (!validFilter(rule.m_filter))
        {
            result.m_conflicts.push_back({rule, rule, "invalid topic filter"});
            seen.push_back(rule);
            continue;
        }
        if (ranges::any_of(existing, [&](const auto& stored) { return sameKey(stored, rule); }))
        {
            result.m_unchanged.push_back(rule);
            seen.push_back(rule);
            continue;
        }
        if (ranges::any_of(existing, [&](const auto& stored)
                           {
                               return stored.m_groupName == rule.m_group.c_str() &&
                                      stored.m_action == rule.m_action.c_str() &&
                                      stored.m_deny == rule.m_deny &&
                                      stored.m_pattern == rule.m_pattern &&
                                      covers(stored.m_filter.c_str(), rule.m_filter);
                           }))
        {
            result.m_redundant.push_back(rule);
            seen.push_back(rule);
            continue;
        }

        bool conflicted = false;
        for (const auto& stored: existing)
        {
            if (stored.m_groupName != rule.m_group.c_str() || stored.m_action != rule.m_action.c_str() ||
                stored.m_pattern != rule.m_pattern || stored.m_deny == rule.m_deny)
            {
                continue;
            }
            if (const auto scope = intersection(stored.m_filter.c_str(), rule.m_filter))
            {
                result.m_conflicts.push_back(
                    {rule,
                     {.m_group = stored.m_groupName.c_str(),
                      .m_action = stored.m_action.c_str(),
                      .m_deny = stored.m_deny,
                      .m_filter = stored.m_filter.c_str(),
                      .m_source = stored.m_source.c_str(),
                      .m_ref = stored.m_sourceRef.c_str(),
                      .m_order = stored.m_order,
                      .m_pattern = stored.m_pattern},
                     *scope});
                conflicted = true;
            }
        }

        if (!conflicted)
        {
            for (const auto& earlier: seen)
            {
                if (earlier.m_group != rule.m_group || earlier.m_action != rule.m_action ||
                    earlier.m_pattern != rule.m_pattern || earlier.m_deny == rule.m_deny)
                {
                    continue;
                }
                if (const auto scope = intersection(earlier.m_filter, rule.m_filter))
                {
                    result.m_conflicts.push_back({rule, earlier, *scope});
                    conflicted = true;
                }
            }
        }

        if (!conflicted)
        {
            result.m_new.push_back(rule);
        }
        seen.push_back(rule);
    }
    return result;
}

vector<UserStore::AclRule> PermissionsImport::apply(const vector<UserStore::AclRule>& existing,
                                                    const Plan& plan, const string_view resolution)
{
    vector<UserStore::AclRule> result = existing;

    if (resolution == "imported")
    {
        for (const auto& conflict: plan.m_conflicts)
        {
            const auto& old = conflict.m_existing;
            erase_if(result, [&](const auto& stored)
                     {
                         return stored.m_groupName == old.m_group.c_str() &&
                                stored.m_action == old.m_action.c_str() &&
                                stored.m_deny == old.m_deny && stored.m_filter == old.m_filter.c_str() &&
                                stored.m_pattern == old.m_pattern;
                     });
            result.push_back({.m_groupName = conflict.m_imported.m_group.c_str(),
                              .m_action = conflict.m_imported.m_action.c_str(),
                              .m_deny = conflict.m_imported.m_deny,
                              .m_filter = conflict.m_imported.m_filter.c_str(),
                              .m_source = conflict.m_imported.m_source.c_str(),
                              .m_sourceRef = conflict.m_imported.m_ref.c_str(),
                              .m_order = conflict.m_imported.m_order,
                              .m_pattern = conflict.m_imported.m_pattern});
        }
    }

    // An unchanged rule keeps its place in the file's order: a re-import after the source was
    // reordered has to move it, or the broker would go on checking the old order.
    for (const auto& rule: plan.m_unchanged)
    {
        for (auto& stored: result)
        {
            if (stored.m_groupName == rule.m_group.c_str() &&
                stored.m_action == rule.m_action.c_str() && stored.m_deny == rule.m_deny &&
                stored.m_filter == rule.m_filter.c_str() && stored.m_pattern == rule.m_pattern)
            {
                stored.m_order = rule.m_order;
                stored.m_source = rule.m_source.c_str();
                stored.m_sourceRef = rule.m_ref.c_str();
            }
        }
    }

    for (const auto& rule: plan.m_new)
    {
        result.push_back({.m_groupName = rule.m_group.c_str(),
                          .m_action = rule.m_action.c_str(),
                          .m_deny = rule.m_deny,
                          .m_filter = rule.m_filter.c_str(),
                          .m_source = rule.m_source.c_str(),
                          .m_sourceRef = rule.m_ref.c_str(),
                          .m_order = rule.m_order,
                          .m_pattern = rule.m_pattern});
    }

    // The store's identity is the canonical key; the plan already deduplicated, but a caller may
    // have passed a set that already contained one of the new rules.
    map<string, UserStore::AclRule> unique;
    for (const auto& rule: result)
    {
        const auto key = string(rule.m_groupName.c_str()) + "|" + rule.m_action.c_str() + "|" +
                         (rule.m_deny ? "1" : "0") + "|" + (rule.m_pattern ? "1" : "0") + "|" +
                         rule.m_filter.c_str();
        unique.emplace(key, rule);
    }
    result.clear();
    for (auto& [key, rule]: unique)
    {
        result.push_back(std::move(rule));
    }
    return result;
}

string PermissionsImport::toJson(const Plan& plan)
{
    string result = "{\"new\":" + arrayJson(plan.m_new) +
                    ",\"unchanged\":" + arrayJson(plan.m_unchanged) +
                    ",\"redundant\":" + arrayJson(plan.m_redundant) + ",\"conflicts\":[";
    for (size_t index = 0; index < plan.m_conflicts.size(); ++index)
    {
        if (index)
        {
            result += ",";
        }
        result += "{\"imported\":" + ruleJson(plan.m_conflicts[index].m_imported) +
                  ",\"existing\":" + ruleJson(plan.m_conflicts[index].m_existing) +
                  ",\"scope\":" + jsonQuote(plan.m_conflicts[index].m_scope) + "}";
    }
    result += "],\"skipped\":[";
    for (size_t index = 0; index < plan.m_skipped.size(); ++index)
    {
        if (index)
        {
            result += ",";
        }
        result += jsonQuote(plan.m_skipped[index]);
    }
    result += "],\"counts\":{\"new\":" + to_string(plan.m_new.size()) +
              ",\"unchanged\":" + to_string(plan.m_unchanged.size()) +
              ",\"redundant\":" + to_string(plan.m_redundant.size()) +
              ",\"conflicts\":" + to_string(plan.m_conflicts.size()) + "}}";
    return result;
}

} // namespace xmq
