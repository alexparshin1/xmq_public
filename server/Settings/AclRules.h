#pragma once

#include "UserStore.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
 * them.
 *
 * The evaluation follows Mosquitto rather than an allow-list per group: every rule that applies to
 * the client is considered, the highest priority wins, and within a priority the earliest position
 * wins. A pattern rule is a template for every client, with %u and %c replaced at check time. When
 * no rule matches but the client has rules at all, the answer is deny - Mosquitto's default for a
 * client the rules know about.
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

    /// Decide for a username and client id, resolving user:<name>, account groups and patterns.
    [[nodiscard]] Decision authorize(std::string_view username, std::string_view clientId,
                                     std::string_view topic, bool publish);

    /// The same decision when the caller already knows the groups, for tests and cached callers.
    [[nodiscard]] Decision authorizeGroups(const std::vector<std::string>& groups, std::string_view username,
                                           std::string_view clientId, std::string_view topic,
                                           bool publish) const;

    /// True when at least one rule is loaded; a false answer leaves the decision to extensions.
    [[nodiscard]] bool empty() const;

    /**
     * @brief The one evaluator, shared by the broker and the Permissions API's "test".
     *
     * `groups` are the subjects that apply to the client, without the pattern pseudo-subject.
     * `explanation`, when given, receives the rule that decided, in words the screen can show.
     */
    static Decision evaluate(const std::vector<UserStore::AclRule>& rules,
                             const std::vector<std::string>& groups,
                             std::string_view username, std::string_view clientId,
                             std::string_view topic, bool publish,
                             std::string* explanation = nullptr);

private:
    mutable std::shared_mutex                m_mutex;
    std::vector<UserStore::AclRule>          m_rules;
    std::unordered_map<std::string, std::vector<std::string>> m_groupsByUser;
    const UserManager*                       m_users {nullptr};
};

} // namespace xmq
