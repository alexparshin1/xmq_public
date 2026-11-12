/* Copyright (c) 2026 Alexey Parshin. MPL-2.0. */
// Standalone harness: drives the same class and host ABI as the shared library.
#include "../Ldap.cpp"
#include <cstdlib>
#include <iostream>
#include <map>

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
