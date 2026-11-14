/* Platform declarations for the LDAP client used by xmq_ldap. */
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <winldap.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstddef>

namespace ldap_platform {
std::wstring utf16(std::string_view value);
std::string utf8(std::wstring_view value);
bool validDn(std::string_view value);
std::string error(unsigned long code);
struct Session {
    LDAP* handle = nullptr;
    ~Session();
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};
int open(Session&, std::string_view host, unsigned short port, bool ldaps,
         unsigned timeoutSeconds, std::string_view caFile);
int startTls(Session&, unsigned timeoutSeconds);
int bind(Session&, std::string_view dn, std::string_view password);
int search(Session&, std::string_view base, int scope, std::string_view filter,
           char** attributes, unsigned sizeLimit, unsigned timeoutSeconds, LDAPMessage** result);
int countEntries(Session&, LDAPMessage*);
LDAPMessage* firstEntry(Session&, LDAPMessage*);
std::string entryDn(Session&, LDAPMessage*);
std::string attribute(Session&, LDAPMessage*, std::string_view name);
void freeMessage(LDAPMessage*);
}
#else
#include <ldap.h>
#include <string>
#include <string_view>
namespace ldap_platform {
inline bool validDn(const std::string_view value)
{
    LDAPDN parsed = nullptr;
    const auto result = ldap_str2dn(std::string(value).c_str(), &parsed, LDAP_DN_FORMAT_LDAPV3);
    if (parsed) ldap_dnfree(parsed);
    return result == LDAP_SUCCESS;
}
inline std::string error(const unsigned long code) { return ldap_err2string(static_cast<int>(code)); }
struct Session {
    LDAP* handle = nullptr;
    ~Session();
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};
int open(Session&, std::string_view host, unsigned short port, bool ldaps,
         unsigned timeoutSeconds, std::string_view caFile);
int startTls(Session&, unsigned timeoutSeconds);
int bind(Session&, std::string_view dn, std::string_view password);
int search(Session&, std::string_view base, int scope, std::string_view filter,
           char** attributes, unsigned sizeLimit, unsigned timeoutSeconds, LDAPMessage** result);
inline int countEntries(Session& s, LDAPMessage* m) { return ldap_count_entries(s.handle, m); }
inline LDAPMessage* firstEntry(Session& s, LDAPMessage* m) { return ldap_first_entry(s.handle, m); }
std::string entryDn(Session&, LDAPMessage*);
std::string attribute(Session&, LDAPMessage*, std::string_view name);
inline void freeMessage(LDAPMessage* m) { if (m) ldap_msgfree(m); }
}
#endif
