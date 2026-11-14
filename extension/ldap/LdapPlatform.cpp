#include "LdapPlatform.h"

#ifndef _WIN32
namespace ldap_platform {
Session::~Session() { if (handle) ldap_unbind_ext_s(handle, nullptr, nullptr); }
int open(Session& session, const std::string_view host, const unsigned short port,
         const bool ldaps, const unsigned timeoutSeconds, const std::string_view caFile)
{
    const std::string uri = std::string(ldaps ? "ldaps://" : "ldap://") +
                            std::string(host) + ":" + std::to_string(port);
    int result = ldap_initialize(&session.handle, uri.c_str());
    if (result != LDAP_SUCCESS) return result;
    int version = LDAP_VERSION3;
    if ((result = ldap_set_option(session.handle, LDAP_OPT_PROTOCOL_VERSION, &version)) != LDAP_SUCCESS) return result;
    if ((result = ldap_set_option(session.handle, LDAP_OPT_REFERRALS, LDAP_OPT_OFF)) != LDAP_SUCCESS) return result;
    timeval timeout {static_cast<decltype(timeval::tv_sec)>(timeoutSeconds), 0};
    if ((result = ldap_set_option(session.handle, LDAP_OPT_NETWORK_TIMEOUT, &timeout)) != LDAP_SUCCESS) return result;
    if ((result = ldap_set_option(session.handle, LDAP_OPT_TIMEOUT, &timeout)) != LDAP_SUCCESS) return result;
    int demand = LDAP_OPT_X_TLS_DEMAND;
    if ((result = ldap_set_option(session.handle, LDAP_OPT_X_TLS_REQUIRE_CERT, &demand)) != LDAP_SUCCESS) return result;
    if (!caFile.empty()) {
        const std::string ca(caFile);
        if ((result = ldap_set_option(session.handle, LDAP_OPT_X_TLS_CACERTFILE, ca.c_str())) != LDAP_SUCCESS) return result;
    }
    int freshContext = 0;
    return ldap_set_option(session.handle, LDAP_OPT_X_TLS_NEWCTX, &freshContext);
}
int startTls(Session& session, const unsigned) { return ldap_start_tls_s(session.handle, nullptr, nullptr); }
int bind(Session& session, const std::string_view dn, const std::string_view password)
{
    berval credential {password.size(), const_cast<char*>(password.data())};
    const std::string name(dn);
    return ldap_sasl_bind_s(session.handle, dn.empty() ? nullptr : name.c_str(), LDAP_SASL_SIMPLE,
                            &credential, nullptr, nullptr, nullptr);
}
int search(Session& session, const std::string_view base, const int scope, const std::string_view filter,
           char** attributes, const unsigned sizeLimit, const unsigned timeoutSeconds, LDAPMessage** result)
{
    const std::string baseString(base), filterString(filter);
    timeval timeout {static_cast<decltype(timeval::tv_sec)>(timeoutSeconds), 0};
    return ldap_search_ext_s(session.handle, baseString.c_str(), scope, filterString.c_str(), attributes,
                             0, nullptr, nullptr, &timeout, sizeLimit, result);
}
std::string entryDn(Session& session, LDAPMessage* entry)
{
    char* value = ldap_get_dn(session.handle, entry);
    if (!value) return {};
    std::string result(value);
    ldap_memfree(value);
    return result;
}
std::string attribute(Session& session, LDAPMessage* entry, const std::string_view name)
{
    const std::string key(name);
    berval** values = ldap_get_values_len(session.handle, entry, key.c_str());
    if (!values) return {};
    std::string result;
    if (values[0]) result.assign(values[0]->bv_val, static_cast<size_t>(values[0]->bv_len));
    ldap_value_free_len(values);
    return result;
}
}
#endif

#ifdef _WIN32
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>
#include <wincrypt.h>

namespace ldap_platform {
namespace {
struct TlsContext { HCERTSTORE roots = nullptr; HCERTCHAINENGINE engine = nullptr; std::wstring hostname; };
std::mutex tlsMutex;
std::map<LDAP*, TlsContext> tlsContexts;

BOOLEAN LDAPAPI verifyServerCertificate(PLDAP connection, PCCERT_CONTEXT certificate)
{
    std::lock_guard lock(tlsMutex);
    const auto found = tlsContexts.find(connection);
    if (found == tlsContexts.end() || !certificate) return FALSE;
    CERT_CHAIN_PARA parameters {};
    parameters.cbSize = sizeof(parameters);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(found->second.engine, certificate, nullptr, found->second.roots, &parameters,
                                 0, nullptr, &chain))
        return FALSE;
    HTTPSPolicyCallbackData https {};
    https.cbStruct = sizeof(https);
    https.dwAuthType = AUTHTYPE_SERVER;
    https.fdwChecks = 0;
    https.pwszServerName = const_cast<wchar_t*>(found->second.hostname.c_str());
    CERT_CHAIN_POLICY_PARA policy {};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &https;
    CERT_CHAIN_POLICY_STATUS status {};
    status.cbSize = sizeof(status);
    const BOOL ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) &&
                    status.dwError == 0;
    CertFreeCertificateChain(chain);
    return ok ? TRUE : FALSE;
}

HCERTSTORE loadRoots(const std::string_view caFile)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                     0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!store) return nullptr;
    if (caFile.empty()) return store;
    const auto path = utf16(caFile);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { CertCloseStore(store, 0); return nullptr; }
    const DWORD length = GetFileSize(file, nullptr);
    std::vector<BYTE> contents(length);
    DWORD read = 0;
    const BOOL readOk = length > 0 && ReadFile(file, contents.data(), length, &read, nullptr) && read == length;
    CloseHandle(file);
    if (!readOk) { CertCloseStore(store, 0); return nullptr; }
    DWORD derLength = 0;
    if (!CryptStringToBinaryA(reinterpret_cast<const char*>(contents.data()), length,
                              CRYPT_STRING_BASE64HEADER, nullptr, &derLength, nullptr, nullptr))
    {
        // A DER-encoded certificate file is also accepted.
        derLength = length;
        if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, contents.data(), length,
                                              CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
        { CertCloseStore(store, 0); return nullptr; }
        return store;
    }
    std::vector<BYTE> der(derLength);
    if (!CryptStringToBinaryA(reinterpret_cast<const char*>(contents.data()), length,
                              CRYPT_STRING_BASE64HEADER, der.data(), &derLength, nullptr, nullptr) ||
        !CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der.data(), derLength,
                                          CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
    { CertCloseStore(store, 0); return nullptr; }
    return store;
}
}

std::wstring utf16(const std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 in LDAP setting");
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), result.data(), size))
        throw std::runtime_error("LDAP UTF-8 conversion failed");
    return result;
}

std::string utf8(const std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("LDAP returned invalid UTF-16");
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("LDAP UTF-16 conversion failed");
    return result;
}

bool validDn(const std::string_view value)
{
    // WinLDAP has no ldap_str2dn. Validate the RFC 4514 structure conservatively: escaped
    // separators are accepted, but empty RDNs/attributes, controls and dangling escapes are not.
    if (value.empty() || value.front() == ',' || value.back() == ',') return false;
    bool escaped = false, hasEquals = false, hasValue = false;
    for (const unsigned char c : value)
    {
        if (c < 0x20 || c == 0x7f) return false;
        if (escaped) { escaped = false; hasValue = true; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '=' && !hasEquals) { hasEquals = true; continue; }
        if (c == ',' || c == '+')
        {
            if (!hasEquals || !hasValue) return false;
            hasEquals = hasValue = false;
            continue;
        }
        if (hasEquals && c != ' ') hasValue = true;
    }
    return !escaped && hasEquals && hasValue;
}

std::string error(const unsigned long code)
{
    const wchar_t* message = ldap_err2stringW(code);
    return message ? utf8(message) : "WinLDAP error " + std::to_string(code);
}

Session::~Session()
{
    if (!handle) return;
    { std::lock_guard lock(tlsMutex); const auto found = tlsContexts.find(handle);
      if (found != tlsContexts.end()) { if (found->second.engine) CertFreeCertificateChainEngine(found->second.engine);
        CertCloseStore(found->second.roots, 0); tlsContexts.erase(found); } }
    ldap_unbind_s(handle);
}

int open(Session& session, const std::string_view host, const unsigned short port, const bool ldaps,
         const unsigned timeoutSeconds, const std::string_view caFile)
{
    const auto server = utf16(host);
    session.handle = ldap_initW(const_cast<PWCHAR>(server.c_str()), port);
    if (!session.handle) return LdapGetLastError();
    ULONG version = LDAP_VERSION3, referrals = 0;
    ULONG result = ldap_set_optionW(session.handle, LDAP_OPT_PROTOCOL_VERSION, &version);
    if (result != LDAP_SUCCESS) return static_cast<int>(result);
    result = ldap_set_optionW(session.handle, LDAP_OPT_REFERRALS, &referrals);
    if (result != LDAP_SUCCESS) return static_cast<int>(result);
    auto roots = loadRoots(caFile);
    if (!roots) return LDAP_LOCAL_ERROR;
    HCERTCHAINENGINE engine = nullptr;
    if (!caFile.empty())
    {
        CERT_CHAIN_ENGINE_CONFIG config {};
        config.cbSize = sizeof(config);
        config.hExclusiveRoot = roots;
        if (!CertCreateCertificateChainEngine(&config, &engine)) { CertCloseStore(roots, 0); return LDAP_LOCAL_ERROR; }
    }
    { std::lock_guard lock(tlsMutex); tlsContexts.emplace(session.handle, TlsContext{roots, engine, server}); }
    result = ldap_set_optionW(session.handle, LDAP_OPT_SERVER_CERTIFICATE,
                              reinterpret_cast<void*>(verifyServerCertificate));
    if (result != LDAP_SUCCESS) return static_cast<int>(result);
    if (ldaps)
    {
        ULONG enabled = 1;
        result = ldap_set_optionW(session.handle, LDAP_OPT_SSL, &enabled);
        if (result != LDAP_SUCCESS) return static_cast<int>(result);
    }
    l_timeval timeout {static_cast<long>(timeoutSeconds), 0};
    return static_cast<int>(ldap_connect(session.handle, &timeout));
}

int startTls(Session& session, const unsigned)
{
    return static_cast<int>(ldap_start_tls_sW(session.handle, nullptr, nullptr, nullptr, nullptr));
}

int bind(Session& session, const std::string_view dn, const std::string_view password)
{
    const auto name = utf16(dn);
    berval credential {password.size(), const_cast<char*>(password.data())};
    return ldap_sasl_bind_sW(session.handle, dn.empty() ? nullptr : const_cast<PWCHAR>(name.c_str()),
                             const_cast<PWCHAR>(L""), &credential, nullptr, nullptr, nullptr);
}

int search(Session& session, const std::string_view base, const int scope, const std::string_view filter,
           char** attributes, const unsigned sizeLimit, const unsigned timeoutSeconds, LDAPMessage** result)
{
    const auto baseWide = utf16(base), filterWide = utf16(filter);
    std::vector<std::wstring> owned;
    for (size_t i = 0; attributes && attributes[i]; ++i) owned.push_back(utf16(attributes[i]));
    attrs.clear();
    for (auto& item : owned) attrs.push_back(item.data());
    attrs.push_back(nullptr);
    l_timeval timeout {static_cast<long>(timeoutSeconds), 0};
    return static_cast<int>(ldap_search_ext_sW(session.handle, const_cast<PWCHAR>(baseWide.c_str()),
        scope, const_cast<PWCHAR>(filterWide.c_str()), attrs.data(), 0, nullptr, nullptr,
        &timeout, sizeLimit, result));
}
int countEntries(Session& session, LDAPMessage* message) { return static_cast<int>(ldap_count_entries(session.handle, message)); }
LDAPMessage* firstEntry(Session& session, LDAPMessage* message) { return ldap_first_entry(session.handle, message); }
std::string entryDn(Session& session, LDAPMessage* entry)
{
    PWCHAR value = ldap_get_dnW(session.handle, entry);
    if (!value) return {};
    std::wstring dn(value); ldap_memfreeW(value); return utf8(dn);
}
std::string attribute(Session& session, LDAPMessage* entry, const std::string_view name)
{
    const auto key = utf16(name);
    berval** values = ldap_get_values_lenW(session.handle, entry, const_cast<PWCHAR>(key.c_str()));
    if (!values) return {};
    std::string value;
    if (values[0]) value.assign(values[0]->bv_val, static_cast<size_t>(values[0]->bv_len));
    ldap_value_free_len(values);
    return value;
}
void freeMessage(LDAPMessage* message) { if (message) ldap_msgfree(message); }
}
#endif
