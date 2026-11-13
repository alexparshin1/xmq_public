/* XMQ LDAP authenticator. Copyright (c) 2026 Alexey Parshin.
 * Mozilla Public License 2.0: https://mozilla.org/MPL/2.0/ */
#include <extension/XmqExtension.h>
#include <ldap.h>

#include <charconv>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <unordered_map>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace {

// RFC 4515: interpolate only escaped assertion values, never a raw MQTT username.
std::string escapeFilter(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte: value)
    {
        // Escaping non-ASCII bytes also handles invalid UTF-8 safely.
        if (byte == 0 || byte == '(' || byte == ')' || byte == '*' || byte == '\\' || byte >= 128)
        {
            result += '\\';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        else
        {
            result += static_cast<char>(byte);
        }
    }
    return result;
}

struct Config
{
    std::string uri, base, bindDn, bindPassword, filter, prefix, caFile;
    bool startTls = true;
    int timeout = 3;
    std::vector<std::pair<std::string, std::string>> mappings;
    std::string groupFilter; ///< Empty means the profile default; see groupSearchFilter().
    int cacheSeconds = 60;
    int negativeCacheSeconds = 10;
    size_t cacheEntries = 4096;
    /// Active Directory: sAMAccountName (or userPrincipalName for a login with @), nested groups
    /// through the in-chain matching rule, and the disabled bit of userAccountControl.
    bool ad = false;
    bool nestedGroups = false;
    bool checkAccountState = false;
    std::string accountStateAttribute = "userAccountControl";
    int accountStateDisabledMask = 2;
};

/// Whether a directory's account-state value marks the account disabled. A value that is not a
/// number is read as "no opinion", never as "disabled": a misconfigured attribute must not lock
/// everybody out.
bool accountDisabled(const std::string_view value, const int mask)
{
    long flags = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), flags);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
    {
        return false;
    }
    return (flags & mask) != 0;
}

/// The filter the user is looked up with. An operator's own user_filter, when set, always wins; the
/// profile decides only what the default is.
std::string userSearchFilter(const Config& config, const std::string_view login)
{
    if (!config.filter.empty()) return config.filter;
    if (!config.ad) return "(uid={username})";
    // A login with @ is a user principal name; a login without one is the short account name.
    return login.find('@') == std::string_view::npos ? "(sAMAccountName={username})"
                                                     : "(userPrincipalName={username})";
}

/// The membership filter used at each mapped group DN. Nested AD groups need the in-chain matching
/// rule, which asks the directory whether the user is a member at any depth.
std::string groupSearchFilter(const Config& config)
{
    if (!config.groupFilter.empty()) return config.groupFilter;
    return config.nestedGroups ? "(member:1.2.840.113556.1.4.1941:={dn})" : "(member={dn})";
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::pair<std::string, std::string>> parseMappings(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        line = trim(line);
        if (line.empty()) continue;
        const auto separator = line.find("=>");
        if (separator == std::string::npos) throw std::runtime_error("LDAP group_mappings needs DN => XMQ-group, one per line");
        auto dn = trim(line.substr(0, separator));
        auto group = trim(line.substr(separator + 2));
        LDAPDN parsed = nullptr;
        const auto valid = ldap_str2dn(dn.c_str(), &parsed, LDAP_DN_FORMAT_LDAPV3);
        if (parsed) ldap_dnfree(parsed);
        if (valid != LDAP_SUCCESS || dn.empty() || group.empty() || group.size() > 80 ||
            group.starts_with("user:") || group == "anonymous" || group.find('\0') != std::string::npos ||
            dn.find('\0') != std::string::npos || result.size() >= 64)
            throw std::runtime_error("LDAP group mapping has an invalid DN/group or exceeds 64 mappings");
        result.emplace_back(std::move(dn), std::move(group));
    }
    return result;
}

void substitute(std::string& text, std::string_view token, const std::string& escaped)
{
    size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos)
    {
        text.replace(position, token.size(), escaped);
        position += escaped.size();
    }
}

void validate(const Config& config)
{
    if (config.uri.find_first_of(" \t\r\n") != std::string::npos ||
        !(config.uri.starts_with("ldap://") || config.uri.starts_with("ldaps://")))
        throw std::runtime_error("LDAP URI must be a single ldap:// or ldaps:// endpoint");
    // Do not silently allow plaintext password binds, even on a test stand.
    if (config.uri.starts_with("ldap://") && !config.startTls)
        throw std::runtime_error("ldap:// requires StartTLS; plaintext password binds are disabled");
    if (config.base.empty() || config.prefix.empty())
        throw std::runtime_error("LDAP search_base and username_prefix must not be empty");
    // An empty filter means the profile's default; one that is set must have exactly one placeholder.
    const auto token = config.filter.find("{username}");
    if (!config.filter.empty() &&
        (token == std::string::npos || config.filter.find("{username}", token + 10) != std::string::npos))
        throw std::runtime_error("LDAP user_filter must contain exactly one {username}");
    if (config.bindDn.empty() != config.bindPassword.empty())
        throw std::runtime_error("LDAP bind_dn and bind_password must both be set or both be empty");
    if (config.timeout < 1 || config.timeout > 30)
        throw std::runtime_error("LDAP timeout_seconds must be between 1 and 30");
}

struct Session
{
    LDAP* handle = nullptr;
    ~Session() { if (handle) ldap_unbind_ext_s(handle, nullptr, nullptr); }
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

void check(int code, const char* operation)
{
    if (code != LDAP_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + ldap_err2string(code));
}

void connect(Session& session, const Config& config)
{
    check(ldap_initialize(&session.handle, config.uri.c_str()), "LDAP initialize");
    int version = LDAP_VERSION3;
    check(ldap_set_option(session.handle, LDAP_OPT_PROTOCOL_VERSION, &version), "LDAP version");
    check(ldap_set_option(session.handle, LDAP_OPT_REFERRALS, LDAP_OPT_OFF), "LDAP referrals");
    timeval timeout {config.timeout, 0};
    check(ldap_set_option(session.handle, LDAP_OPT_NETWORK_TIMEOUT, &timeout), "LDAP network timeout");
    check(ldap_set_option(session.handle, LDAP_OPT_TIMEOUT, &timeout), "LDAP operation timeout");
    int demand = LDAP_OPT_X_TLS_DEMAND;
    check(ldap_set_option(session.handle, LDAP_OPT_X_TLS_REQUIRE_CERT, &demand), "LDAP certificate verification");
    if (!config.caFile.empty())
        check(ldap_set_option(session.handle, LDAP_OPT_X_TLS_CACERTFILE, config.caFile.c_str()), "LDAP CA file");
    // Per-session context: never change the process-wide TLS policy of other extensions.
    int freshContext = 0;
    check(ldap_set_option(session.handle, LDAP_OPT_X_TLS_NEWCTX, &freshContext), "LDAP TLS context");
    if (config.uri.starts_with("ldap://"))
        check(ldap_start_tls_s(session.handle, nullptr, nullptr), "LDAP StartTLS");
}

int bind(Session& session, const std::string& dn, const std::string& password)
{
    berval credential {password.size(), const_cast<char*>(password.data())};
    return ldap_sasl_bind_s(session.handle, dn.empty() ? nullptr : dn.c_str(), LDAP_SASL_SIMPLE,
                            &credential, nullptr, nullptr, nullptr);
}

/// A decision is keyed by identity and a per-process HMAC of the password: a hit can be answered
/// without holding the password, and a login that presents a different password is a different key,
/// so it is checked with the directory at once. A password changed at the directory is not visible
/// while an entry lives: the cache cannot know, and the lifetime bounds how long that lasts.
/// An empty key means "no cache": entropy was unavailable at start, so remember nothing.
std::string decisionKey(const std::array<unsigned char, 32>& salt, const std::string_view username,
                        const std::string& password)
{
    if (std::all_of(salt.begin(), salt.end(), [](const auto byte) { return byte == 0; }))
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (HMAC(EVP_sha256(), salt.data(), static_cast<int>(salt.size()),
             reinterpret_cast<const unsigned char*>(password.data()), password.size(), digest,
             &length) == nullptr)
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string key(username);
    key += '\0';
    for (unsigned int index = 0; index < length; ++index)
    {
        key += hex[digest[index] >> 4];
        key += hex[digest[index] & 15];
    }
    return key;
}

class DecisionCache
{
public:
    void configure(const int positives, const int negatives, const size_t maxEntries)
    {
        const std::scoped_lock lock(m_mutex);
        m_positive = std::chrono::seconds(std::max(0, positives));
        m_negative = std::chrono::seconds(std::max(0, negatives));
        m_maxEntries = std::max<size_t>(1, maxEntries);
        m_entries.clear();
        m_order.clear();
    }

    void clear()
    {
        const std::scoped_lock lock(m_mutex);
        m_entries.clear();
        m_order.clear();
    }

    bool find(const std::string& key, xmq_auth_decision& decision, std::vector<std::string>& groups)
    {
        const std::scoped_lock lock(m_mutex);
        const auto found = m_entries.find(key);
        if (found == m_entries.end()) return false;
        if (found->second.first.expires <= std::chrono::steady_clock::now())
        {
            m_order.erase(found->second.second);
            m_entries.erase(found);
            return false;
        }
        m_order.splice(m_order.begin(), m_order, found->second.second);
        decision = found->second.first.decision;
        groups = found->second.first.groups;
        return true;
    }

    /// ALLOW keeps its groups; a refusal is remembered only briefly so a corrected password is not
    /// held out for the positive lifetime. SUBSYSTEM_ERROR is never stored: an outage must not
    /// become a remembered answer in either direction. The guard is here as well as at the call
    /// sites, so a future caller cannot turn an outage into a cached verdict by accident.
    void store(const std::string& key, const xmq_auth_decision decision, const std::vector<std::string>& groups)
    {
        if (decision == XMQ_AUTH_SUBSYSTEM_ERROR) return;
        const std::scoped_lock lock(m_mutex);
        const auto lifetime = decision == XMQ_AUTH_ALLOW ? m_positive : m_negative;
        if (lifetime.count() <= 0) return;
        if (const auto existing = m_entries.find(key); existing != m_entries.end())
        {
            existing->second.first = {decision, groups, std::chrono::steady_clock::now() + lifetime};
            m_order.splice(m_order.begin(), m_order, existing->second.second);
            return;
        }
        if (m_entries.size() >= m_maxEntries) evict();
        m_order.push_front(key);
        m_entries.emplace(key, std::make_pair(CacheEntry {decision, groups,
                                                          std::chrono::steady_clock::now() + lifetime},
                                              m_order.begin()));
    }

private:
    struct CacheEntry
    {
        xmq_auth_decision decision;
        std::vector<std::string> groups;
        std::chrono::steady_clock::time_point expires;
    };

    void evict()
    {
        // Expired first, then the least recently used: an identity that reconnects keeps its place.
        const auto now = std::chrono::steady_clock::now();
        for (auto entry = m_entries.begin(); entry != m_entries.end();)
        {
            if (entry->second.first.expires <= now)
            {
                m_order.erase(entry->second.second);
                entry = m_entries.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
        if (!m_entries.empty())
        {
            m_entries.erase(m_order.back());
            m_order.pop_back();
        }
    }

    std::mutex m_mutex;
    std::unordered_map<std::string, std::pair<CacheEntry, std::list<std::string>::iterator>> m_entries;
    std::list<std::string> m_order;
    std::chrono::seconds m_positive {60};
    std::chrono::seconds m_negative {10};
    size_t m_maxEntries {4096};
};

class LdapAuthenticator final : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    static constexpr bool NativeGroups = true;

    bool start() override
    {
        // A per-process salt, so a cached decision can be keyed by the password without keeping the
        // password: a different broker process (or a restart) shares no keys with this one.
        if (RAND_bytes(m_salt.data(), static_cast<int>(m_salt.size())) != 1)
        {
            m_salt.fill(0);
            return false;
        }
        return reload();
    }

    bool reload() override
    {
        try
        {
            auto config = std::make_shared<Config>();
            // Host, port and encryption rather than a URI: the form asks for what an operator knows,
            // and the scheme follows from the encryption choice instead of being typed by hand.
            const auto host = setting("host");
            if (host.empty())
                throw std::runtime_error("LDAP host is required");
            if (host.find_first_of(" \t/\\:") != std::string::npos ||
                host.find("://") != std::string::npos)
                throw std::runtime_error("LDAP host must be a bare host name, without a scheme, port or path");
            const auto encryption = setting("encryption", "ldaps");
            if (encryption != "ldaps" && encryption != "starttls")
                throw std::runtime_error("LDAP encryption must be ldaps or starttls");
            config->startTls = encryption == "starttls";
            const auto portText = setting("port", config->startTls ? "389" : "636");
            int        port = 0;
            const auto portParsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
            if (portParsed.ec != std::errc() || portParsed.ptr != portText.data() + portText.size() ||
                port < 1 || port > 65535)
                throw std::runtime_error("LDAP port must be a number between 1 and 65535");
            config->uri = std::string(config->startTls ? "ldap://" : "ldaps://") + host + ":" +
                          std::to_string(port);
            config->base = setting("search_base");
            config->bindDn = setting("bind_dn");
            config->bindPassword = setting("bind_password");
            // Empty means "whatever this directory profile defaults to" - see userSearchFilter().
            config->filter = setting("user_filter");
            config->prefix = setting("username_prefix", "ldap/");
            config->caFile = setting("ca_file");
            config->mappings = parseMappings(setting("group_mappings"));
            config->groupFilter = setting("group_filter");
            if (!config->groupFilter.empty() &&
                config->groupFilter.find("{dn}") == std::string::npos &&
                config->groupFilter.find("{username}") == std::string::npos)
                throw std::runtime_error("LDAP group_filter needs {dn} or {username}");
            const auto directoryType = setting("directory_type", "ldap");
            if (directoryType != "ldap" && directoryType != "ad")
                throw std::runtime_error("LDAP directory_type must be ldap or ad");
            config->ad = directoryType == "ad";
            const auto automatic = [this, &directoryType](const char* name, const bool adDefault) -> bool
            {
                const auto choice = setting(name, "auto");
                if (choice == "auto") return directoryType == "ad" && adDefault;
                if (choice == "yes") return true;
                if (choice == "no") return false;
                throw std::runtime_error(std::string("LDAP ") + name + " must be auto, yes or no");
            };
            config->nestedGroups = automatic("nested_groups", true);
            config->checkAccountState = automatic("account_state_check", true);
            config->accountStateAttribute = setting("account_state_attribute", "userAccountControl");
            const auto mask = setting("account_state_disabled_mask", "2");
            const auto maskParsed = std::from_chars(mask.data(), mask.data() + mask.size(),
                                                    config->accountStateDisabledMask);
            if (maskParsed.ec != std::errc() || maskParsed.ptr != mask.data() + mask.size() ||
                config->accountStateDisabledMask < 0)
                throw std::runtime_error("LDAP account_state_disabled_mask must be a non-negative integer");
            if (config->checkAccountState && config->accountStateAttribute.empty())
                throw std::runtime_error("LDAP account_state_attribute must not be empty when the check is on");
            const auto seconds = setting("timeout_seconds", "3");
            const auto parsed = std::from_chars(seconds.data(), seconds.data() + seconds.size(), config->timeout);
            if (parsed.ec != std::errc() || parsed.ptr != seconds.data() + seconds.size())
                throw std::runtime_error("LDAP timeout_seconds must be an integer");
            const auto number = [this](const char* name, const char* fallback) -> int
            {
                const auto text = setting(name, fallback);
                int value = 0;
                const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                if (result.ec != std::errc() || result.ptr != text.data() + text.size())
                    throw std::runtime_error(std::string("LDAP ") + name + " must be an integer");
                return value;
            };
            config->cacheSeconds = std::clamp(number("cache_ttl_seconds", "60"), 0, 3600);
            config->negativeCacheSeconds =
                std::clamp(number("negative_cache_ttl_seconds", "10"), 0, 3600);
            config->cacheEntries = static_cast<size_t>(std::clamp(number("cache_max_entries", "4096"), 1, 1000000));
            validate(*config);
            const std::lock_guard lock(m_mutex);
            m_config = std::move(config);
            m_cache.configure(m_config->cacheSeconds, m_config->negativeCacheSeconds,
                              m_config->cacheEntries);
            return true;
        }
        catch (const std::exception& error)
        {
            log(XMQ_LOG_ERROR, error.what());
            return false;
        }
    }

    xmq_auth_decision authenticate(const xmq_auth_request& request) override
    {
        std::vector<std::string> ignored;
        return authenticateWithGroups(request, ignored);
    }

    xmq_auth_decision authenticateWithGroups(const xmq_auth_request& request,
                                             std::vector<std::string>& groups) override
    {
        groups.clear();
        std::shared_ptr<const Config> config;
        { const std::lock_guard lock(m_mutex); config = m_config; }
        if (!config) return XMQ_AUTH_SUBSYSTEM_ERROR;
        const std::string_view username(request.username.data ? request.username.data : "", request.username.length);
        if (!username.starts_with(config->prefix)) return XMQ_AUTH_NOT_HANDLED;
        if (username.size() == config->prefix.size() || request.password.length == 0 || !request.password.data)
            return XMQ_AUTH_DENY; // Empty passwords can be accepted as unauthenticated binds by LDAP.
        const std::string password(request.password.data, request.password.length);
        const std::string key = cacheKey(username, password);
        if (!key.empty())
        {
            xmq_auth_decision cached {};
            std::vector<std::string> cachedGroups;
            if (m_cache.find(key, cached, cachedGroups))
            {
                groups = std::move(cachedGroups);
                return cached;
            }
        }
        auto filter = userSearchFilter(*config, username.substr(config->prefix.size()));
        const auto remember = [this, &key](const xmq_auth_decision decision)
        {
            if (!key.empty()) m_cache.store(key, decision, {});
            return decision;
        };
        filter.replace(filter.find("{username}"), 10, escapeFilter(username.substr(config->prefix.size())));
        try
        {
            Session search;
            connect(search, *config);
            check(bind(search, config->bindDn, config->bindPassword), "LDAP search-account bind");
            LDAPMessage* raw = nullptr;
            timeval timeout {config->timeout, 0};
            char noAttributes[] = "1.1";
            char* attributes[] = {noAttributes, nullptr};
            const auto code = ldap_search_ext_s(search.handle, config->base.c_str(), LDAP_SCOPE_SUBTREE,
                filter.c_str(), attributes, 0, nullptr, nullptr, &timeout, 2, &raw);
            const std::unique_ptr<LDAPMessage, decltype(&ldap_msgfree)> result(raw, ldap_msgfree);
            // Multiple matches are ambiguous identities, never pick the first account.
            if (code == LDAP_SIZELIMIT_EXCEEDED) return remember(XMQ_AUTH_DENY);
            check(code, "LDAP user search");
            const auto count = ldap_count_entries(search.handle, result.get());
            if (count == 0) return remember(XMQ_AUTH_NOT_HANDLED);
            if (count != 1) return remember(XMQ_AUTH_DENY);
            char* rawDn = ldap_get_dn(search.handle, ldap_first_entry(search.handle, result.get()));
            if (!rawDn) throw std::runtime_error("LDAP user search returned no DN");
            const std::string dn(rawDn);
            ldap_memfree(rawDn);
            Session user;
            connect(user, *config);
            const auto bindCode = bind(user, dn, password);
            if (bindCode == LDAP_INVALID_CREDENTIALS) return remember(XMQ_AUTH_DENY);
            check(bindCode, "LDAP user bind");

            // A bind proves the password, not that the account may be used: AD can accept the bind
            // and still carry the disabled flag. Read that flag as the service account, which can
            // see it, and refuse rather than admit an account the directory has switched off.
            if (config->checkAccountState)
            {
                LDAPMessage* stateRaw = nullptr;
                char        stateAttribute[128];
                std::snprintf(stateAttribute, sizeof(stateAttribute), "%s",
                              config->accountStateAttribute.c_str());
                char* stateAttributes[] = {stateAttribute, nullptr};
                const auto stateCode = ldap_search_ext_s(search.handle, dn.c_str(), LDAP_SCOPE_BASE,
                    "(objectClass=*)", stateAttributes, 0, nullptr, nullptr, &timeout, 1, &stateRaw);
                const std::unique_ptr<LDAPMessage, decltype(&ldap_msgfree)> state(stateRaw, ldap_msgfree);
                check(stateCode, "LDAP account state search");
                if (ldap_count_entries(search.handle, state.get()) == 1)
                {
                    auto* entry = ldap_first_entry(search.handle, state.get());
                    berval** values = ldap_get_values_len(search.handle, entry,
                                                          config->accountStateAttribute.c_str());
                    if (values != nullptr && values[0] != nullptr)
                    {
                        const auto disabled = accountDisabled(
                            std::string_view(values[0]->bv_val, static_cast<size_t>(values[0]->bv_len)),
                            config->accountStateDisabledMask);
                        ldap_value_free_len(values);
                        if (disabled) return remember(XMQ_AUTH_DENY);
                    }
                }
            }

            // Query membership only after the user's password was verified. Search as the
            // read-only service identity, on the same trusted directory, with no referrals.
            auto groupFilter = groupSearchFilter(*config);
            substitute(groupFilter, "{dn}", escapeFilter(dn));
            substitute(groupFilter, "{username}", escapeFilter(username.substr(config->prefix.size())));
            for (const auto& [groupDn, nativeGroup]: config->mappings)
            {
                LDAPMessage* membershipRaw = nullptr;
                const auto status = ldap_search_ext_s(search.handle, groupDn.c_str(), LDAP_SCOPE_BASE,
                    groupFilter.c_str(), attributes, 0, nullptr, nullptr, &timeout, 1, &membershipRaw);
                const std::unique_ptr<LDAPMessage, decltype(&ldap_msgfree)> membership(membershipRaw, ldap_msgfree);
                check(status, "LDAP group membership search");
                if (ldap_count_entries(search.handle, membership.get()) == 1 &&
                    std::find(groups.begin(), groups.end(), nativeGroup) == groups.end())
                    groups.push_back(nativeGroup);
            }
            if (!key.empty()) m_cache.store(key, XMQ_AUTH_ALLOW, groups);
            return XMQ_AUTH_ALLOW;
        }
        catch (const std::exception& error)
        {
            // No DNs, passwords, filters or server-supplied diagnostic strings in the log.
            log(XMQ_LOG_ERROR, error.what());
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }
    }

private:
    /// A decision is keyed by identity and a per-process HMAC of the password: a hit can be
    /// answered without holding the password, and a changed password is a miss by construction.
    std::string cacheKey(const std::string_view username, const std::string& password) const
    {
        return decisionKey(m_salt, username, password);
    }

    std::mutex m_mutex;
    std::shared_ptr<const Config> m_config;
    DecisionCache                 m_cache;
    std::array<unsigned char, 32> m_salt {};
};
} // namespace

const xmq_setting ldapSettings[] {
    {.name="host", .label="LDAP host", .description="Host name of the directory server, without ldap://, a port or a path", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=1},
    {.name="port", .label="LDAP port", .description="Port of the directory server. Empty uses 636 for LDAPS and 389 for StartTLS", .type=XMQ_SETTING_INTEGER, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="encryption", .label="LDAP encryption", .description="LDAPS connects with TLS from the first byte; StartTLS upgrades a plain connection before any password is sent", .type=XMQ_SETTING_CHOICE, .default_value="ldaps", .choices="ldaps,starttls", .required=0},
    {.name="directory_type", .label="Directory type", .description="ldap for OpenLDAP and other LDAP servers; ad for Active Directory, which logs in by sAMAccountName or user principal name, follows nested groups and reads the account-state flag", .type=XMQ_SETTING_CHOICE, .default_value="ldap", .choices="ldap,ad", .required=0},
    {.name="nested_groups", .label="Nested groups", .description="auto follows nested groups for Active Directory and direct membership otherwise; yes uses the in-chain matching rule, no reads direct membership only", .type=XMQ_SETTING_CHOICE, .default_value="auto", .choices="auto,yes,no", .required=0},
    {.name="account_state_check", .label="Check account state", .description="auto reads the account-state attribute for Active Directory and skips it otherwise; yes refuses an account whose flag says disabled, no relies on the bind alone", .type=XMQ_SETTING_CHOICE, .default_value="auto", .choices="auto,yes,no", .required=0},
    {.name="account_state_attribute", .label="Account state attribute", .description="Attribute that carries the account flags; userAccountControl in Active Directory", .type=XMQ_SETTING_STRING, .default_value="userAccountControl", .choices=nullptr, .required=0},
    {.name="account_state_disabled_mask", .label="Disabled flag mask", .description="Bit that marks a disabled account; 2 is ACCOUNTDISABLE in Active Directory", .type=XMQ_SETTING_INTEGER, .default_value="2", .choices=nullptr, .required=0},
    {.name="ca_file", .label="Directory CA certificate", .description="The certificate authority that signed the directory server's certificate - not this broker's own certificate. Empty uses the system trust store", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="search_base", .label="Search base DN", .description="Subtree containing users", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=1},
    {.name="bind_dn", .label="Search account DN", .description="Read-only search account; empty for anonymous search", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="bind_password", .label="Search account password", .description="Password of the search account", .type=XMQ_SETTING_SECRET, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="user_filter", .label="User search filter", .description="RFC 4515 filter with exactly one {username} placeholder. Empty follows the directory type: uid for ldap, sAMAccountName or userPrincipalName for ad", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="username_prefix", .label="LDAP username prefix", .description="Nonempty namespace; other usernames pass to local authentication", .type=XMQ_SETTING_STRING, .default_value="ldap/", .choices=nullptr, .required=0},
    {.name="timeout_seconds", .label="Operation timeout (seconds)", .description="Timeout per network operation, 1-30 seconds", .type=XMQ_SETTING_INTEGER, .default_value="3", .choices=nullptr, .required=0},
    {.name="group_mappings", .label="LDAP to XMQ groups", .description="One LDAP group DN => existing XMQ group per line; at most 64", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="group_filter", .label="Group membership filter", .description="Filter at each mapped group DN; {dn} is user DN, {username} is the unprefixed login. Empty follows nested_groups", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="cache_ttl_seconds", .label="Cache verified logins (seconds)", .description="How long a successful directory login and its groups are remembered. 0 disables the cache, so every CONNECT checks the directory and a revoked password stops working at once", .type=XMQ_SETTING_INTEGER, .default_value="60", .choices=nullptr, .required=0},
    {.name="negative_cache_ttl_seconds", .label="Cache refused logins (seconds)", .description="How long a wrong password, an unknown account or an ambiguous search is remembered; 0 disables it. It limits password guessing at the cost of delaying a corrected password", .type=XMQ_SETTING_INTEGER, .default_value="10", .choices=nullptr, .required=0},
    {.name="cache_max_entries", .label="Cached identities", .description="Upper bound on remembered logins, least recently used evicted first", .type=XMQ_SETTING_INTEGER, .default_value="4096", .choices=nullptr, .required=0}
};

// Starting points rather than separate configurations: a directory type decides the search filter,
// the group matching and the account check that belong with it, and a filter left over from a
// previous directory silently overrides the profile. Only those fields are filled - host, search
// base, the search account, the CA and the group mappings are the site's and are left as they are,
// as are the timeouts and the cache, which are tuning rather than profile.
const xmq_setting_value adPresetValues[] {
    {.name="directory_type", .value="ad"},
    {.name="encryption", .value="ldaps"},
    {.name="port", .value="636"},
    {.name="username_prefix", .value="ldap/"},
    {.name="nested_groups", .value="auto"},
    {.name="account_state_check", .value="auto"},
    {.name="user_filter", .value=""},
    {.name="group_filter", .value=""}
};
const xmq_setting_value ldapPresetValues[] {
    {.name="directory_type", .value="ldap"},
    {.name="encryption", .value="ldaps"},
    {.name="port", .value="636"},
    {.name="username_prefix", .value="ldap/"},
    {.name="nested_groups", .value="auto"},
    {.name="account_state_check", .value="auto"},
    {.name="user_filter", .value=""},
    {.name="group_filter", .value=""}
};
const xmq_setting_preset ldapPresets[] {
    {.name="ad", .label="Active Directory", .description="Active Directory: login by sAMAccountName or user principal name, nested groups, and the account-state flag. Fills the directory type, LDAPS on 636, the ldap/ prefix and the profile switches; leaves host, search base, the search account, the CA, group mappings, timeouts and the cache as they are", .values=adPresetValues, .value_count=sizeof(adPresetValues) / sizeof(adPresetValues[0])},
    {.name="ldap", .label="LDAP", .description="OpenLDAP and other LDAP servers: login by uid and direct group membership. Fills the directory type, LDAPS on 636, the ldap/ prefix and the profile switches; leaves host, search base, the search account, the CA, group mappings, timeouts and the cache as they are", .values=ldapPresetValues, .value_count=sizeof(ldapPresetValues) / sizeof(ldapPresetValues[0])}
};
XMQ_DEFINE_EXTENSION_WITH_PRESETS(LdapAuthenticator, "ldap", "0.1", XMQ_CAP_AUTHENTICATOR,
    "LDAP bind authentication over verified TLS; explicit username namespace", ldapSettings, ldapPresets)
