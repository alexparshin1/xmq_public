#include "AclRules.h"

#include "UserManager.h"
#include "UserStore.h"
#include "server/Subscription/RetainedMessages.h"

#include <mutex>

using namespace std;
using namespace sptk;

namespace xmq {

void AclRules::reload(const shared_ptr<DatabaseConnectionPool>& database, const UserManager& users)
{
    const unique_lock lock(m_mutex);

    m_users = &users;
    m_rules.clear();
    m_groupsWithRules.clear();
    m_groupsByUser.clear();

    if (!database)
    {
        return;
    }

    UserStore store(database);
    store.createSchema();
    for (const auto& stored: store.aclRules())
    {
        m_rules.push_back({stored.m_groupName.c_str(), stored.m_filter.c_str(),
                           stored.m_action == "publish", stored.m_deny});
        m_groupsWithRules.insert(stored.m_groupName.c_str());
    }
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

AclRules::Decision AclRules::authorize(const string_view username, const string_view topic, const bool publish)
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
        if (!name.empty())
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
        groups.push_back("anonymous");
        cached = m_groupsByUser.emplace(name, std::move(groups)).first;
    }

    return authorizeUnlocked(cached->second, topic, publish);
}

AclRules::Decision AclRules::authorizeGroups(const vector<string>& groups, const string_view topic,
                                             const bool publish) const
{
    const shared_lock lock(m_mutex);
    return authorizeUnlocked(groups, topic, publish);
}

AclRules::Decision AclRules::authorizeUnlocked(const vector<string>& groups, const string_view topic,
                                               const bool publish) const
{
    for (const auto& group: groups)
    {
        // A group with no rules is not this evaluator's business: say nothing and let the next
        // group, an extension, or the broker's own rules answer.
        if (!m_groupsWithRules.contains(group))
        {
            continue;
        }

        bool allowed = false;
        for (const auto& rule: m_rules)
        {
            if (rule.m_group != group || rule.m_publish != publish ||
                !RetainedMessages::matches(rule.m_filter, topic))
            {
                continue;
            }
            if (rule.m_deny)
            {
                return Decision::Deny;
            }
            allowed = true;
        }
        // The first group that has rules decides, as resolveGroup() stops at the first matching
        // group. Within it, no matching allow is a denial: the group's rules are an allow-list.
        return allowed ? Decision::Allow : Decision::Deny;
    }
    return Decision::NotHandled;
}

} // namespace xmq
