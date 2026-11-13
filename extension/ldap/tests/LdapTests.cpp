/* Copyright (c) 2026 Alexey Parshin. MPL-2.0. */
// Standalone harness: drives the same class and host ABI as the shared library.
#include "../Ldap.cpp"
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <thread>

namespace {
std::map<std::string, std::string> settings;
void logMessage(void*, xmq_log_priority, const char* message) { std::cerr << message << '\n'; }
int64_t getSetting(void*, const char* key, char* buffer, size_t capacity)
{
    const auto found = settings.find(key);
    if (found == settings.end()) return -1;
    const auto size = found->second.size() + 1;
    if (buffer && capacity >= size) std::memcpy(buffer, found->second.c_str(), size);
    return static_cast<int64_t>(size - 1);
}
void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
xmq_auth_decision auth(LdapAuthenticator& extension, std::string_view user, std::string_view password)
{
    xmq_auth_request request {};
    request.username = {user.data(), user.size()};
    request.password = {password.data(), password.size()};
    return extension.authenticate(request);
}
}

int main(int argc, char** argv)
{
    try
    {
        expect(escapeFilter("a*)(uid=*)\\") == "a\\2a\\29\\28uid=\\2a\\29\\5c", "filter injection escaping");
        expect(escapeFilter(std::string("x\0y", 3)) == "x\\00y", "NUL escaping");
        expect(parseMappings("cn=writers,dc=example,dc=org => Operators\n").size() == 1, "mapping parser");

        // Directory profile: the default lookup follows the type, and an operator's filter wins.
        Config plain;
        expect(userSearchFilter(plain, "alice") == "(uid={username})", "ldap default filter");
        Config ad;
        ad.ad = true;
        expect(userSearchFilter(ad, "alice") == "(sAMAccountName={username})", "AD short name");
        expect(userSearchFilter(ad, "alice@corp.example") == "(userPrincipalName={username})", "AD UPN");
        Config custom;
        custom.ad = true;
        custom.filter = "(cn={username})";
        expect(userSearchFilter(custom, "alice") == "(cn={username})", "operator filter overrides the profile");

        // Nested groups: the in-chain matching rule only when asked for.
        Config direct;
        expect(groupSearchFilter(direct) == "(member={dn})", "direct membership by default");
        Config nested;
        nested.nestedGroups = true;
        expect(groupSearchFilter(nested) == "(member:1.2.840.113556.1.4.1941:={dn})", "nested membership");

        // Account flags: 514 is ACCOUNTDISABLE|NORMAL_ACCOUNT, 512 is NORMAL_ACCOUNT.
        expect(accountDisabled("514", 2), "disabled flag read");
        expect(!accountDisabled("512", 2), "enabled account");
        expect(!accountDisabled("not-a-number", 2), "unreadable state is not a refusal");

        // Presets: a button that fills the form must never fill it with something Apply refuses.
        // Every name has to be a declared setting, every value valid for that setting's type, and
        // no preset may carry a secret - it is written in the library, in plain sight.
        {
            std::map<std::string, const xmq_setting*> declared;
            for (const auto& setting: ldapSettings)
                declared[setting.name] = &setting;
            const auto proceeds = [](const char* text)
            {
                return std::all_of(text, text + std::strlen(text),
                                   [](const unsigned char symbol) { return std::isdigit(symbol) != 0; });
            };
            expect(sizeof(ldapPresets) / sizeof(ldapPresets[0]) == 2, "an AD and an LDAP preset");
            for (const auto& preset: ldapPresets)
            {
                expect(preset.name != nullptr && preset.label != nullptr && preset.description != nullptr,
                       "a preset describes itself");
                expect(preset.values != nullptr && preset.value_count > 0, "a preset fills something");
                for (size_t at = 0; at < preset.value_count; ++at)
                {
                    const auto& value = preset.values[at];
                    const auto found = declared.find(value.name);
                    expect(found != declared.end(), "a preset names a setting the extension declares");
                    const auto& setting = *found->second;
                    expect(setting.type != XMQ_SETTING_SECRET, "a preset never carries a secret");
                    if (setting.type == XMQ_SETTING_CHOICE && !std::string_view(value.value).empty())
                    {
                        const std::string choices = std::string(",") + setting.choices + ",";
                        expect(choices.find(std::string(",") + value.value + ",") != std::string::npos,
                               "a preset chooses one of the declared options");
                    }
                    if (setting.type == XMQ_SETTING_INTEGER && !std::string_view(value.value).empty())
                        expect(proceeds(value.value), "a preset gives an integer setting a number");
                }
                // A profile linked to a choice has to name a real option of a real choice setting,
                // or selecting that option would fill the form from nothing.
                expect((preset.choice_setting == nullptr) == (preset.choice_value == nullptr),
                       "a linked preset names both the choice and the option");
                if (preset.choice_setting != nullptr)
                {
                    const auto found = declared.find(preset.choice_setting);
                    expect(found != declared.end() && found->second->type == XMQ_SETTING_CHOICE,
                           "a linked preset points at a declared choice");
                    const std::string choices = std::string(",") + found->second->choices + ",";
                    expect(choices.find(std::string(",") + preset.choice_value + ",") != std::string::npos,
                           "a linked preset names one of the declared options");
                }
            }
            // Reading the table through the ABI is what the broker does; a mismatch here would
            // leave the screen with no presets at all.
            const auto* table = xmq_extension_describe(XMQ_EXTENSION_ABI_MAJOR, XMQ_EXTENSION_ABI_MINOR);
            expect(table != nullptr, "the extension describes itself at this ABI");
            expect(table->preset_count == 2 && table->presets == ldapPresets,
                   "the described table carries the presets");
        }

        // The cache key: identity plus a keyed hash of the password, never the password itself.
        std::array<unsigned char, 32> salt {};
        salt.fill(7);
        const auto key = decisionKey(salt, "ldap/alice", "secret");
        expect(!key.empty() && key == decisionKey(salt, "ldap/alice", "secret"), "same login, same key");
        expect(key != decisionKey(salt, "ldap/alice", "secret2"), "a changed password is a different key");
        expect(key != decisionKey(salt, "ldap/bob", "secret"), "another identity is a different key");
        expect(key.find("secret") == std::string::npos, "the key does not carry the password");
        expect(decisionKey(std::array<unsigned char, 32> {}, "ldap/alice", "secret").empty(),
               "without entropy there is no key, so nothing is cached");

        // Decision cache: lifetimes, the outage rule, and the eviction order.
        {
            DecisionCache cache;
            xmq_auth_decision decision {};
            std::vector<std::string> groups;
            cache.configure(60, 10, 4096);
            cache.store("allow", XMQ_AUTH_ALLOW, {"Operators"});
            expect(cache.find("allow", decision, groups) && decision == XMQ_AUTH_ALLOW && groups.size() == 1,
                   "an allow is remembered with its groups");
            cache.store("deny", XMQ_AUTH_DENY, {});
            expect(cache.find("deny", decision, groups) && decision == XMQ_AUTH_DENY, "a refusal is remembered");
            cache.store("outage", XMQ_AUTH_SUBSYSTEM_ERROR, {});
            expect(!cache.find("outage", decision, groups), "an outage is never remembered");

            // A zero lifetime disables that direction only.
            cache.configure(0, 10, 4096);
            cache.store("fresh", XMQ_AUTH_ALLOW, {});
            expect(!cache.find("fresh", decision, groups), "zero positive lifetime caches no allow");
            cache.store("wrong", XMQ_AUTH_DENY, {});
            expect(cache.find("wrong", decision, groups), "refusals keep their own lifetime");

            // Least recently used goes first, and a hit keeps an entry alive.
            cache.configure(60, 10, 2);
            cache.store("x", XMQ_AUTH_ALLOW, {});
            cache.store("y", XMQ_AUTH_ALLOW, {});
            expect(cache.find("x", decision, groups), "precondition: both entries are present");
            cache.store("z", XMQ_AUTH_ALLOW, {});
            expect(cache.find("x", decision, groups) && !cache.find("y", decision, groups) &&
                   cache.find("z", decision, groups), "the least recently used entry is evicted");

            // Reload replaces the cache, so a changed directory policy is not held out.
            cache.configure(60, 10, 4096);
            expect(!cache.find("z", decision, groups), "configuring a new policy drops remembered decisions");

            // Expiry: both directions fall out of the cache on their own lifetime.
            cache.configure(1, 1, 4096);
            cache.store("young-allow", XMQ_AUTH_ALLOW, {});
            cache.store("young-deny", XMQ_AUTH_DENY, {});
            expect(cache.find("young-allow", decision, groups) && cache.find("young-deny", decision, groups),
                   "precondition: young entries hit");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            expect(!cache.find("young-allow", decision, groups) && !cache.find("young-deny", decision, groups),
                   "expired entries miss and are dropped");
        }
        Config invalid {.uri="ldap://localhost", .base="dc=example,dc=org", .filter="(uid={username})", .prefix="ldap/", .startTls=false};
        bool rejected = false;
        try { validate(invalid); } catch (...) { rejected = true; }
        expect(rejected, "plaintext binds must be rejected");
        xmq_host host {};
        host.struct_size = sizeof(host);
        host.setting = getSetting;
        host.log = logMessage;
        settings = {{"host", "127.0.0.1"}, {"port", "1"}, {"encryption", "ldaps"},
                    {"search_base", "dc=example,dc=org"}, {"timeout_seconds", "1"}};
        LdapAuthenticator extension(host);
        expect(extension.start(), "valid settings accepted");
        expect(auth(extension, "admin", "secret") == XMQ_AUTH_NOT_HANDLED, "local administrator bypasses LDAP");
        expect(auth(extension, "ldap/alice", "") == XMQ_AUTH_DENY, "empty password denied before network");
        expect(auth(extension, "ldap/", "secret") == XMQ_AUTH_DENY, "empty directory username denied");
        expect(auth(extension, "ldap/alice", "secret") == XMQ_AUTH_SUBSYSTEM_ERROR, "outage is not wrong password");
        settings["username_prefix"] = "";
        expect(!extension.reload(), "empty namespace rejected");
        expect(auth(extension, "admin", "secret") == XMQ_AUTH_NOT_HANDLED, "failed reload preserves previous configuration");
        settings.erase("username_prefix");
        if (argc == 5)
        {
            settings["host"] = argv[1];
            settings["port"] = argv[2];
            settings["encryption"] = argv[3];
            settings["ca_file"] = argv[4];
            settings["bind_dn"] = "cn=admin,dc=example,dc=org";
            settings["bind_password"] = "test-admin";
            settings["group_mappings"] = "cn=publishers,dc=example,dc=org => Publishers\ncn=readers,dc=example,dc=org => Readers";
            expect(extension.reload(), "integration settings");
            expect(auth(extension, "ldap/alice", "test-alice") == XMQ_AUTH_ALLOW, "valid directory password");
            expect(auth(extension, "ldap/alice", "wrong") == XMQ_AUTH_DENY, "invalid directory password");
            expect(auth(extension, "ldap/missing", "wrong") == XMQ_AUTH_NOT_HANDLED, "unknown account abstains");
            expect(auth(extension, "ldap/duplicate", "test-duplicate") == XMQ_AUTH_DENY, "ambiguous search must deny");
            std::vector<std::string> groups;
            xmq_auth_request identityRequest {};
            identityRequest.username = {"ldap/alice", 10};
            identityRequest.password = {"test-alice", 10};
            expect(extension.authenticateWithGroups(identityRequest, groups) == XMQ_AUTH_ALLOW, "identity authentication");
            expect(groups == std::vector<std::string>{"Publishers"}, "only actual membership is mapped");
            const auto* table = xmq_extension_describe(XMQ_EXTENSION_ABI_MAJOR, XMQ_EXTENSION_ABI_MINOR);
            expect(table && table->authenticate_identity, "identity ABI exported");
            groups.clear();
            const xmq_identity_sink sink {&groups, [](void* context, const xmq_str* name) -> xmq_status {
                static_cast<std::vector<std::string>*>(context)->emplace_back(name->data, name->length);
                return XMQ_OK;
            }};
            expect(table->authenticate_identity(&extension, &identityRequest, &sink) == XMQ_AUTH_ALLOW, "C ABI authentication");
            expect(groups == std::vector<std::string>{"Publishers"}, "C ABI emits copied group names");
            settings["group_mappings"] = "cn=readers,dc=example,dc=org => Readers";
            expect(extension.reload(), "mapping reload");
            expect(extension.authenticateWithGroups(identityRequest, groups) == XMQ_AUTH_ALLOW && groups.empty(), "no membership does not invent Default group");
            expect(auth(extension, "ldap/*)(uid=alice", "test-alice") == XMQ_AUTH_NOT_HANDLED, "filter injection does not match alice");
            settings["bind_password"] = "wrong";
            expect(extension.reload(), "new search password settings");
            expect(auth(extension, "ldap/alice", "test-alice") == XMQ_AUTH_SUBSYSTEM_ERROR, "bad service password is subsystem failure");
            settings["bind_password"] = "test-admin";
            settings["ca_file"] = "/nonexistent-xmq-ldap-ca.pem";
            expect(extension.reload(), "untrusted CA settings");
            expect(auth(extension, "ldap/alice", "test-alice") == XMQ_AUTH_SUBSYSTEM_ERROR, "TLS trust failure never admits user");
            settings["ca_file"] = argv[4];
            const std::string goodHost = argv[1];
            settings["host"] = goodHost == "127.0.0.1" ? "localhost" : "127.0.0.1";
            expect(extension.reload(), "mismatched hostname settings");
            expect(auth(extension, "ldap/alice", "test-alice") == XMQ_AUTH_SUBSYSTEM_ERROR, "TLS hostname mismatch rejected");
            settings["host"] = goodHost;
            expect(extension.reload(), "restore directory settings");
            expect(auth(extension, "ldap/alice", "test-alice") == XMQ_AUTH_ALLOW, "recovery without cached outage");
        }
        std::cout << "LDAP tests passed" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << std::endl;
        return 1;
    }
}
