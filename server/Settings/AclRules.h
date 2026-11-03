#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sptk {
class DatabaseConnectionPool;
}

namespace xmq {

class UserManager;

/**
 * @brief Topic permissions from the accounts database, evaluated for the broker.
 *
 * Loaded from xmq_acl_rule and refreshed when the Permissions API changes a rule. The groups a
 * client belongs to are resolved once per username and cached; a change to membership invalidates
 * them. A group that has rules is an allow-list: no matching allow is a denial, which is the same
 * rule a Mosquitto ACL file has.
 */
class AclRules
{
public:
    enum class Decision
    {
        NotHandled,
        Allow,
        Deny
    };

    /// Replace the rule set. `users` is borrowed and must outlive this object.
    void reload(const std::shared_ptr<sptk::DatabaseConnectionPool>& database, const UserManager& users);

    /// Drop the cached username-to-groups answers; rules stay loaded.
    void invalidateGroups();

    /// Decide for a username, resolving user:<name>, account groups, then anonymous.
    [[nodiscard]] Decision authorize(std::string_view username, std::string_view topic, bool publish);

    /// The same decision when the caller already knows the groups, for tests and callers that cache.
    [[nodiscard]] Decision authorizeGroups(const std::vector<std::string>& groups, std::string_view topic,
                                           bool publish) const;

    /// True when at least one rule is loaded; a false answer leaves the decision to extensions.
    [[nodiscard]] bool empty() const;

private:
    [[nodiscard]] Decision authorizeUnlocked(const std::vector<std::string>& groups, std::string_view topic,
                                             bool publish) const;

    struct Rule
    {
        std::string m_group;
        std::string m_filter;
        bool        m_publish {true};
        bool        m_deny {false};
    };

    mutable std::shared_mutex                m_mutex;
    std::vector<Rule>                        m_rules;
    std::unordered_set<std::string>          m_groupsWithRules;
    std::unordered_map<std::string, std::vector<std::string>> m_groupsByUser;
    const UserManager*                       m_users {nullptr};
};

} // namespace xmq
