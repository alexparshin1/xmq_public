#include "PermissionsImport.h"

#include <algorithm>
#include <cctype>
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
        // The same rule the control API applies: no empty levels, one whole level for a wildcard.
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

string join(const vector<string>& parts, string_view separator);

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
           ",\"source\":" + jsonQuote(rule.m_source) + ",\"ref\":" + jsonQuote(rule.m_ref) + "}";
}

string ruleJson(const UserStore::AclRule& rule)
{
    return "{\"group\":" + jsonQuote(rule.m_groupName.c_str()) + ",\"action\":" + jsonQuote(rule.m_action.c_str()) +
           ",\"deny\":" + (rule.m_deny ? "true" : "false") + ",\"filter\":" + jsonQuote(rule.m_filter.c_str()) +
           ",\"source\":" + jsonQuote(rule.m_source.c_str()) + ",\"ref\":" + jsonQuote(rule.m_sourceRef.c_str()) + "}";
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

} // namespace

vector<PermissionsImport::Rule> PermissionsImport::parseAcl(const string_view content,
                                                            vector<string>&  skipped)
{
    vector<Rule> rules;
    string       current = "anonymous";
    set<string>  users;
    struct Pattern
    {
        string action;
        bool   deny;
        string filter;
        size_t line;
    };
    vector<Pattern> patterns;

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
            if (!name.empty())
            {
                users.insert(current);
            }
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

        if (keyword == "pattern")
        {
            for (const auto& [action, deny]: actions)
            {
                patterns.push_back({action, deny, filter, lineNumber});
            }
            continue;
        }
        for (const auto& [action, deny]: actions)
        {
            rules.push_back({current, action, deny, filter, "mosquitto",
                             "acl:" + to_string(lineNumber)});
        }
    }

    for (const auto& pattern: patterns)
    {
        if (pattern.filter.find("%c") != string::npos)
        {
            skipped.push_back("line " + to_string(pattern.line) + ": '%c' is not supported by the authorizer");
            continue;
        }
        vector<string> targets;
        if (pattern.filter.find("%u") != string::npos)
        {
            targets.assign(users.begin(), users.end());
        }
        else
        {
            targets.assign(users.begin(), users.end());
            targets.push_back("anonymous");
        }
        for (const auto& group: targets)
        {
            auto expanded = pattern.filter;
            const auto user = group.starts_with("user:") ? group.substr(5) : "";
            for (auto at = expanded.find("%u"); at != string::npos && !user.empty();
                 at = expanded.find("%u", at + user.size()))
            {
                expanded.replace(at, 2, user);
            }
            if (!validFilter(expanded))
            {
                skipped.push_back("line " + to_string(pattern.line) + ": invalid expanded filter '" + expanded + "'");
                continue;
            }
            rules.push_back({group, pattern.action, pattern.deny, expanded, "mosquitto",
                             "acl:" + to_string(pattern.line)});
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

    const auto defaults = root->findFirst("defaultACLAccess");
    const bool defaultPublish = defaults && defaults->getBoolean("publishClientSend");
    const bool defaultSubscribe = defaults && defaults->getBoolean("subscribe");
    const auto anonymousGroup = root->findFirst("anonymousGroup") ? root->getString("anonymousGroup") : String("unauthenticated");

    vector<Rule> rules;

    const auto addRule = [&rules](const string& group, const string& action, const bool deny,
                                  const string& filter, const string& ref)
    {
        rules.push_back({group, action, deny, filter, "dynamic-security", ref});
    };

    const auto roleRules = [&](const string& rolename, const string& username, const string& label)
    {
        const auto found = roles.find(rolename);
        if (found == roles.end())
        {
            skipped.push_back(label + ": role '" + rolename + "' not found");
            return;
        }
        for (const auto& acl: found->second->nodes("acls"))
        {
            const string acltype = acl->getString("acltype").c_str();
            string       topic = acl->getString("topic").c_str();
            const bool allow = acl->getBoolean("allow");
            const auto ref = label + "/role/" + rolename;
            if (topic.empty())
            {
                skipped.push_back(ref + ": empty topic");
                continue;
            }
            if (string(topic).find("%c") != string::npos)
            {
                skipped.push_back(ref + ": '%c' is not supported by the authorizer");
                continue;
            }
            if (!username.empty())
            {
                for (auto at = topic.find("%u"); at != string::npos; at = topic.find("%u", at + username.size()))
                {
                    topic.replace(at, 2, username);
                }
            }
            if (!validFilter(topic))
            {
                skipped.push_back(ref + ": invalid filter '" + topic + "'");
                continue;
            }
            const string group = "user:" + username;
            if (acltype == "publishClientSend")
            {
                addRule(group, "publish", !allow, topic, ref);
            }
            else if (acltype == "subscribeLiteral" || acltype == "subscribePattern")
            {
                if (acltype == "subscribeLiteral")
                {
                    skipped.push_back(ref + ": subscribeLiteral is approximated as a filter");
                }
                addRule(group, "subscribe", !allow, topic, ref);
            }
            else if (acltype == "publishClientReceive")
            {
                if (!allow)
                {
                    skipped.push_back(ref + ": publishClientReceive deny is carried as a subscribe deny");
                    addRule(group, "subscribe", true, topic, ref);
                }
                else
                {
                    skipped.push_back(ref + ": publishClientReceive allow is implied by subscribe");
                }
            }
            else if (acltype.starts_with("unsubscribe"))
            {
                skipped.push_back(ref + ": " + string(acltype) + " has no equivalent");
            }
            else
            {
                skipped.push_back(ref + ": unknown acltype '" + string(acltype) + "'");
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
        const auto label = "client/" + string(username);
        for (const auto& assignment: client->nodes("roles"))
        {
            roleRules(assignment->getString("rolename").c_str(), username, label + "/direct");
        }
        for (const auto& assignment: client->nodes("groups"))
        {
            const string groupname = assignment->getString("groupname").c_str();
            const auto found = groups.find(groupname);
            if (found == groups.end())
            {
                skipped.push_back(label + ": group '" + groupname + "' not found");
                continue;
            }
            for (const auto& role: found->second->nodes("roles"))
            {
                roleRules(role->getString("rolename").c_str(), username, label + "/group/" + groupname);
            }
        }
        const bool hasPublish = ranges::any_of(rules, [&](const auto& rule)
                                               { return rule.m_group == "user:" + string(username) && rule.m_action == "publish"; });
        const bool hasSubscribe = ranges::any_of(rules, [&](const auto& rule)
                                                 { return rule.m_group == "user:" + string(username) && rule.m_action == "subscribe"; });
        if (defaultPublish && !hasPublish)
        {
            addRule("user:" + string(username), "publish", false, "#", "default/publishClientSend");
        }
        if (defaultSubscribe && !hasSubscribe)
        {
            addRule("user:" + string(username), "subscribe", false, "#", "default/subscribe");
        }
    }

    // The group Mosquitto uses for clients without a username.
    const auto anonymous = groups.find(anonymousGroup.c_str());
    if (anonymous != groups.end())
    {
        for (const auto& role: anonymous->second->nodes("roles"))
        {
            const string rolename = role->getString("rolename").c_str();
            const auto found = roles.find(rolename);
            if (found == roles.end())
            {
                skipped.push_back("anonymous: role '" + string(rolename) + "' not found");
                continue;
            }
            for (const auto& acl: found->second->nodes("acls"))
            {
                string topic = acl->getString("topic").c_str();
                if (topic.empty() || string(topic).find("%u") != string::npos ||
                    string(topic).find("%c") != string::npos)
                {
                    skipped.push_back("anonymous: a pattern needs a user");
                    continue;
                }
                const string acltype = acl->getString("acltype").c_str();
                const bool allow = acl->getBoolean("allow");
                if (acltype == "publishClientSend")
                {
                    addRule("anonymous", "publish", !allow, topic, "anonymous/" + string(rolename));
                }
                else if (acltype == "subscribeLiteral" || acltype == "subscribePattern")
                {
                    addRule("anonymous", "subscribe", !allow, topic, "anonymous/" + string(rolename));
                }
            }
        }
    }

    skipped.push_back("priorities are flattened: XMQ applies deny-wins, stricter than Mosquitto's first-match order");
    return rules;
}

PermissionsImport::Plan PermissionsImport::plan(const vector<UserStore::AclRule>& existing,
                                                const vector<Rule>&               imported)
{
    Plan result;

    const auto sameKey = [](const UserStore::AclRule& stored, const Rule& rule)
    {
        return stored.m_groupName == rule.m_group.c_str() && stored.m_action == rule.m_action.c_str() &&
               stored.m_deny == rule.m_deny && stored.m_filter == rule.m_filter.c_str();
    };

    for (const auto& rule: imported)
    {
        if (!validFilter(rule.m_filter))
        {
            result.m_conflicts.push_back({rule, rule, "invalid topic filter"});
            continue;
        }
        if (ranges::any_of(existing, [&](const auto& stored) { return sameKey(stored, rule); }))
        {
            result.m_unchanged.push_back(rule);
            continue;
        }
        if (ranges::any_of(existing, [&](const auto& stored)
                           {
                               return stored.m_groupName == rule.m_group.c_str() &&
                                      stored.m_action == rule.m_action.c_str() &&
                                      stored.m_deny == rule.m_deny &&
                                      covers(stored.m_filter.c_str(), rule.m_filter);
                           }))
        {
            result.m_redundant.push_back(rule);
            continue;
        }

        bool conflicted = false;
        for (const auto& stored: existing)
        {
            if (stored.m_groupName != rule.m_group.c_str() || stored.m_action != rule.m_action.c_str() ||
                stored.m_deny == rule.m_deny)
            {
                continue;
            }
            if (const auto scope = intersection(stored.m_filter.c_str(), rule.m_filter))
            {
                result.m_conflicts.push_back(
                    {rule,
                     {stored.m_groupName.c_str(), stored.m_action.c_str(), stored.m_deny,
                      stored.m_filter.c_str(), stored.m_source.c_str(), stored.m_sourceRef.c_str()},
                     *scope});
                conflicted = true;
            }
        }
        if (!conflicted)
        {
            result.m_new.push_back(rule);
        }
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
                                stored.m_deny == old.m_deny && stored.m_filter == old.m_filter.c_str();
                     });
            result.push_back({.m_groupName = conflict.m_imported.m_group.c_str(),
                              .m_action = conflict.m_imported.m_action.c_str(),
                              .m_deny = conflict.m_imported.m_deny,
                              .m_filter = conflict.m_imported.m_filter.c_str(),
                              .m_source = conflict.m_imported.m_source.c_str(),
                              .m_sourceRef = conflict.m_imported.m_ref.c_str()});
        }
    }

    for (const auto& rule: plan.m_new)
    {
        result.push_back({.m_groupName = rule.m_group.c_str(),
                          .m_action = rule.m_action.c_str(),
                          .m_deny = rule.m_deny,
                          .m_filter = rule.m_filter.c_str(),
                          .m_source = rule.m_source.c_str(),
                          .m_sourceRef = rule.m_ref.c_str()});
    }

    // The store's identity is the canonical key; the plan already deduplicated, but a caller may
    // have passed a set that already contained one of the new rules.
    map<string, UserStore::AclRule> unique;
    for (const auto& rule: result)
    {
        const auto key = string(rule.m_groupName.c_str()) + "|" + rule.m_action.c_str() + "|" +
                         (rule.m_deny ? "1" : "0") + "|" + rule.m_filter.c_str();
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
