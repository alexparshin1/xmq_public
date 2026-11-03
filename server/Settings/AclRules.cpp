#include "AclRules.h"

#include "UserManager.h"
#include "UserStore.h"
#include "server/Subscription/RetainedMessages.h"

#include <algorithm>
#include <mutex>
#include <optional>

namespace ranges = std::ranges;

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/// A value may not add levels or wildcards to a pattern: "devices/%c/#" with client id "+" must not
/// become "devices/+/#". Such a client gets no pattern rule, and is denied if it has any rules.
bool usableInPattern(const string_view value)
{
    return value.find_first_of("+#/") == string_view::npos;
}

/**
 * @brief The filter with %u and %c replaced, or nothing when a token has no value.
 *
 * `%u` without a username and `%c` without a client id are not "empty text": Mosquitto does not
 * match those rules, and neither does this.
 */
optional<string> expand(const string_view filter, const string_view username, const string_view clientId)
{
    string expanded;
    expanded.reserve(filter.size());
    for (size_t at = 0; at < filter.size(); ++at)
    {
        if (filter[at] == '%' && at + 1 < filter.size() &&
            (filter[at + 1] == 'u' || filter[at + 1] == 'c'))
        {
            const bool user = filter[at + 1] == 'u';
            const auto value = user ? username : clientId;
            if (value.empty() || !usableInPattern(value))
            {
                return nullopt;
            }
            expanded.append(value);
            ++at;
            continue;
        }
        expanded += filter[at];
    }
    return expanded;
}

} // namespace

void AclRules::reload(const shared_ptr<DatabaseConnectionPool>& database, const UserManager& users)
{
    const unique_lock lock(m_mutex);

    m_users = &users;
    m_rules.clear();
    m_groupsByUser.clear();

    if (!database)
    {
        return;
    }

    UserStore store(database);
    store.createSchema();
    m_rules = store.aclRules();
    m_defaultDeny = store.defaultDeny();
}

void AclRules::invalidateGroups()
{
    const unique_lock lock(m_mutex);
    m_groupsByUser.clear();
}

bool AclRules::empty() const
{
    const shared_lock lock(m_mutex);
    return m_rules.empty();
}

AclRules::Decision AclRules::authorize(const string_view username, const string_view clientId,
                                       const string_view topic, const bool publish)
{
    const unique_lock lock(m_mutex);
    if (m_rules.empty())
    {
        return Decision::NotHandled;
    }

    const string name(username);
    auto         cached = m_groupsByUser.find(name);
    if (cached == m_groupsByUser.end())
    {
        vector<string> groups;
        if (name.empty())
        {
            // Only an unnamed client is in the anonymous group. It used to be a fallback for every
            // user, which made Mosquitto's anonymous rules apply to named clients.
            groups.push_back("anonymous");
        }
        else
        {
            groups.push_back("user:" + name);
            if (m_users)
            {
                for (const auto& group: m_users->groupsOf(name))
                {
                    groups.emplace_back(group.c_str());
                }
            }
        }
        cached = m_groupsByUser.emplace(name, std::move(groups)).first;
    }

    return evaluate(m_rules, cached->second, username, clientId, topic, publish, m_defaultDeny);
}

AclRules::Decision AclRules::authorizeGroups(const vector<string>& groups, const string_view username,
                                             const string_view clientId, const string_view topic,
                                             const bool publish) const
{
    const shared_lock lock(m_mutex);
    return evaluate(m_rules, groups, username, clientId, topic, publish, m_defaultDeny);
}

AclRules::Decision AclRules::evaluate(const vector<UserStore::AclRule>& rules, const vector<string>& groups,
                                      const string_view username, const string_view clientId,
                                      const string_view topic, const bool publish,
                                      const bool defaultDeny, string* explanation)
{
    const UserStore::AclRule* winner = nullptr;
    bool                     managed = false;

    for (size_t index = 0; index < rules.size(); ++index)
    {
        const auto& rule = rules[index];
        const bool applies = rule.m_pattern ||
            ranges::any_of(groups, [&rule](const auto& group) { return rule.m_groupName == group.c_str(); });
        if (!applies)
        {
            continue;
        }
        managed = true;
        if ((rule.m_action == "publish") != publish)
        {
            continue;
        }

        const auto filter = expand(rule.m_filter.c_str(), username, clientId);
        if (!filter || !RetainedMessages::matches(*filter, topic))
        {
            continue;
        }

        if (winner == nullptr || rule.m_order < winner->m_order ||
            (rule.m_order == winner->m_order && rule.m_deny && !winner->m_deny))
        {
            winner = &rule;
        }
    }

    if (winner == nullptr)
    {
        // A client the rules know about but for which nothing matched is denied: that is Mosquitto's
        // default, and it is what keeps a group with rules an allow-list. A client with no rules at
        // all is left to the extensions, precisely as before.
        return (managed || defaultDeny) ? Decision::Deny : Decision::NotHandled;
    }

    if (explanation)
    {
        const auto subject = winner->m_pattern ? string("all clients")
            : (winner->m_groupName == "anonymous" ? string("anonymous") : string(winner->m_groupName.c_str()));
        *explanation = (winner->m_deny ? "denied by " : "allowed by ") + string(winner->m_filter.c_str()) +
            " in " + subject;
    }
    return winner->m_deny ? Decision::Deny : Decision::Allow;
}

} // namespace xmq
