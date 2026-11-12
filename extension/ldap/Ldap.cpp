/* XMQ LDAP authenticator. Copyright (c) 2026 Alexey Parshin.
 * Mozilla Public License 2.0: https://mozilla.org/MPL/2.0/ */
#include <extension/XmqExtension.h>
#include <ldap.h>

#include <charconv>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <algorithm>

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
    std::string groupFilter = "(member={dn})";
};

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
    const auto token = config.filter.find("{username}");
    if (token == std::string::npos || config.filter.find("{username}", token + 10) != std::string::npos)
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

class LdapAuthenticator final : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    static constexpr bool NativeGroups = true;

    bool start() override { return reload(); }

    bool reload() override
    {
        try
        {
            auto config = std::make_shared<Config>();
            config->uri = setting("uri");
            config->base = setting("search_base");
            config->bindDn = setting("bind_dn");
            config->bindPassword = setting("bind_password");
            config->filter = setting("user_filter", "(uid={username})");
            config->prefix = setting("username_prefix", "ldap/");
            config->caFile = setting("ca_file");
            config->mappings = parseMappings(setting("group_mappings"));
            config->groupFilter = setting("group_filter", "(member={dn})");
            if (config->groupFilter.find("{dn}") == std::string::npos &&
                config->groupFilter.find("{username}") == std::string::npos)
                throw std::runtime_error("LDAP group_filter needs {dn} or {username}");
            const auto startTls = setting("start_tls", "true");
            if (startTls != "true" && startTls != "false")
                throw std::runtime_error("LDAP start_tls must be true or false");
            config->startTls = startTls == "true";
            const auto seconds = setting("timeout_seconds", "3");
            const auto parsed = std::from_chars(seconds.data(), seconds.data() + seconds.size(), config->timeout);
            if (parsed.ec != std::errc() || parsed.ptr != seconds.data() + seconds.size())
                throw std::runtime_error("LDAP timeout_seconds must be an integer");
            validate(*config);
            const std::lock_guard lock(m_mutex);
            m_config = std::move(config);
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
        auto filter = config->filter;
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
            if (code == LDAP_SIZELIMIT_EXCEEDED) return XMQ_AUTH_DENY;
            check(code, "LDAP user search");
            const auto count = ldap_count_entries(search.handle, result.get());
            if (count == 0) return XMQ_AUTH_NOT_HANDLED;
            if (count != 1) return XMQ_AUTH_DENY;
            char* rawDn = ldap_get_dn(search.handle, ldap_first_entry(search.handle, result.get()));
            if (!rawDn) throw std::runtime_error("LDAP user search returned no DN");
            const std::string dn(rawDn);
            ldap_memfree(rawDn);
            Session user;
            connect(user, *config);
            const auto bindCode = bind(user, dn, password);
            if (bindCode == LDAP_INVALID_CREDENTIALS) return XMQ_AUTH_DENY;
            check(bindCode, "LDAP user bind");
            // Query membership only after the user's password was verified. Search as the
            // read-only service identity, on the same trusted directory, with no referrals.
            auto groupFilter = config->groupFilter;
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
    std::mutex m_mutex;
    std::shared_ptr<const Config> m_config;
};
} // namespace

const xmq_setting ldapSettings[] {
    {.name="uri", .label="LDAP URI", .description="One ldap:// (StartTLS) or ldaps:// endpoint", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=1},
    {.name="search_base", .label="Search base DN", .description="Subtree containing users", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=1},
    {.name="bind_dn", .label="Search account DN", .description="Read-only search account; empty for anonymous search", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="bind_password", .label="Search account password", .description="Password of the search account", .type=XMQ_SETTING_SECRET, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="user_filter", .label="User search filter", .description="RFC 4515 filter with exactly one {username} placeholder", .type=XMQ_SETTING_STRING, .default_value="(uid={username})", .choices=nullptr, .required=0},
    {.name="username_prefix", .label="LDAP username prefix", .description="Nonempty namespace; other usernames pass to local authentication", .type=XMQ_SETTING_STRING, .default_value="ldap/", .choices=nullptr, .required=0},
    {.name="ca_file", .label="CA certificate file", .description="Trusted directory CA; empty uses system trust", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="start_tls", .label="StartTLS", .description="Mandatory for ldap://; ldaps:// already uses TLS", .type=XMQ_SETTING_CHOICE, .default_value="true", .choices="true,false", .required=0},
    {.name="timeout_seconds", .label="Operation timeout", .description="Timeout per network operation, 1–30 seconds", .type=XMQ_SETTING_INTEGER, .default_value="3", .choices=nullptr, .required=0},
    {.name="group_mappings", .label="LDAP to XMQ groups", .description="One LDAP group DN => existing XMQ group per line; at most 64", .type=XMQ_SETTING_STRING, .default_value=nullptr, .choices=nullptr, .required=0},
    {.name="group_filter", .label="Group membership filter", .description="Filter at each mapped group DN; {dn} is user DN, {username} is the unprefixed login", .type=XMQ_SETTING_STRING, .default_value="(member={dn})", .choices=nullptr, .required=0}
};
XMQ_DEFINE_EXTENSION_DESCRIBED(LdapAuthenticator, "ldap", "0.1", XMQ_CAP_AUTHENTICATOR,
    "LDAP bind authentication over verified TLS; explicit username namespace", ldapSettings)
