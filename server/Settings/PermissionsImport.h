#pragma once

#include "UserStore.h"

#include <string>
#include <string_view>
#include <vector>

namespace xmq {

/**
 * @brief Plan and apply an import of Mosquitto topic rules.
 *
 * Both Mosquitto formats reduce to the same canonical rule. An import is a plan first: every rule
 * is new, unchanged, redundant or a conflict, duplicates are recognised by their canonical key, and
 * nothing is written while a conflict is unresolved.
 *
 * Each rule carries the position at which Mosquitto would check it, so the broker's evaluation can
 * follow the source rather than a house order. A pattern rule keeps its %u and %c tokens; they are
 * replaced when the rule is checked, not when it is imported.
 */
class PermissionsImport
{
public:
    struct Rule
    {
        std::string m_group;
        std::string m_action; ///< "publish" or "subscribe".
        bool        m_deny {false};
        std::string m_filter;
        std::string m_source;
        std::string m_ref;
        int         m_order {0};      ///< Evaluation position, lowest first.
        bool        m_pattern {false}; ///< The filter is a template for every client.
    };

    struct Conflict
    {
        Rule        m_imported;
        Rule        m_existing;
        std::string m_scope;
        bool        m_same_file {false}; ///< The two rules came from the file being imported.
    };

    struct Plan
    {
        std::vector<Rule>     m_new;
        std::vector<Rule>     m_unchanged;
        std::vector<Rule>     m_redundant;
        std::vector<Conflict> m_conflicts;
        std::vector<std::string> m_skipped;
    };

    /// Read a Mosquitto `acl_file`. Lines that cannot be represented are returned in `skipped`.
    static std::vector<Rule> parseAcl(std::string_view content, std::vector<std::string>& skipped);

    /// Read a Mosquitto `dynamic-security.json`.
    static std::vector<Rule> parseDynamicSecurity(std::string_view content, std::vector<std::string>& skipped);

    /// Compare imported rules with the rules already stored.
    static Plan plan(const std::vector<UserStore::AclRule>& existing, const std::vector<Rule>& imported);

    /**
     * @brief Resolve conflicts as asked and return the complete rule set to store.
     *
     * `resolution` is "imported" (the imported rule replaces the conflicting one), "existing" or
     * "skip" (the imported rule is dropped). The caller applies the result atomically.
     */
    static std::vector<UserStore::AclRule> apply(const std::vector<UserStore::AclRule>& existing,
                                                 const Plan& plan, std::string_view resolution);

    /// The plan as JSON, for the control API and the Permissions screen.
    static std::string toJson(const Plan& plan);
};

} // namespace xmq
