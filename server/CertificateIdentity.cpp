#include "CertificateIdentity.h"

#include <memory>
#include <stdexcept>
#include <vector>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace xmq {
namespace {
std::string identity(X509* certificate, const std::string& field)
{
    std::vector<std::string> values;
    const auto append = [&values](const ASN1_STRING* value)
    {
        unsigned char* data = nullptr;
        const auto size = ASN1_STRING_to_UTF8(&data, value);
        const auto freeText = [](unsigned char* text) { OPENSSL_free(text); };
        std::unique_ptr<unsigned char, decltype(freeText)> textOwner(data, freeText);
        if (size <= 0 || size > 1024)
        {
            throw std::runtime_error("Invalid certificate identity length");
        }
        std::string text(reinterpret_cast<const char*>(data), static_cast<size_t>(size));
        for (const auto ch : text)
        {
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7f)
            {
                throw std::runtime_error("Invalid certificate identity character");
            }
        }
        values.push_back(std::move(text));
    };
    if (field == "cn")
    {
        const auto subject = X509_get_subject_name(certificate);
        for (int index = -1; (index = X509_NAME_get_index_by_NID(subject, NID_commonName, index)) >= 0;)
        {
            append(X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index)));
        }
    }
    else if (field == "san_uri" || field == "san_email")
    {
        std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(
            static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)),
            GENERAL_NAMES_free);
        for (int i = 0; names && i < sk_GENERAL_NAME_num(names.get()); ++i)
        {
            const auto name = sk_GENERAL_NAME_value(names.get(), i);
            if (name->type == (field == "san_uri" ? GEN_URI : GEN_EMAIL))
            {
                append(name->d.ia5);
            }
        }
    }
    else
    {
        throw std::runtime_error("Unknown certificate identity field");
    }
    if (values.size() != 1)
    {
        throw std::runtime_error("Certificate must contain exactly one selected identity");
    }
    return values.front();
}
}

void verifyCertificateIdentity(const std::string& chainPem, const std::string& caFile,
                               const std::string& crlFile, const std::string& identityField,
                               const std::string& username, const int verifyDepth)
{
    // PEM parsing reports EOF through OpenSSL's thread-local error queue. Do not leave it
    // behind for a TLS operation that next uses this completion worker.
    struct ClearErrors
    {
        ~ClearErrors() { ERR_clear_error(); }
    } clearErrors;
    ERR_clear_error();
    if (verifyDepth <= 0 || caFile.empty() || chainPem.empty() || chainPem.size() > 1024 * 1024)
    {
        throw std::runtime_error("Certificate policy requires a peer certificate, CA file and positive verify depth");
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> input(
        BIO_new_mem_buf(chainPem.data(), static_cast<int>(chainPem.size())), BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> leaf(
        input ? PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr) : nullptr, X509_free);
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
    const auto freeChain = [](STACK_OF(X509)* chain) { sk_X509_pop_free(chain, X509_free); };
    std::unique_ptr<STACK_OF(X509), decltype(freeChain)> chain(sk_X509_new_null(), freeChain);
    if (!leaf || !store || !context || !chain || X509_STORE_load_locations(store.get(), caFile.c_str(), nullptr) != 1)
    {
        throw std::runtime_error("Cannot load peer certificate or trusted CA");
    }
    while (auto certificate = PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr))
    {
        if (sk_X509_push(chain.get(), certificate) == 0)
        {
            X509_free(certificate);
            throw std::runtime_error("Cannot load peer chain");
        }
    }
    if (!crlFile.empty())
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(crlFile.c_str(), "r"), BIO_free);
        if (!file)
        {
            throw std::runtime_error("Cannot load certificate revocation list");
        }
        size_t count = 0;
        while (auto raw = PEM_read_bio_X509_CRL(file.get(), nullptr, nullptr, nullptr))
        {
            std::unique_ptr<X509_CRL, decltype(&X509_CRL_free)> crl(raw, X509_CRL_free);
            const auto lastUpdate = X509_CRL_get0_lastUpdate(crl.get());
            const auto nextUpdate = X509_CRL_get0_nextUpdate(crl.get());
            if (!lastUpdate || !nextUpdate || X509_cmp_current_time(lastUpdate) >= 0 ||
                X509_cmp_current_time(nextUpdate) <= 0 || X509_STORE_add_crl(store.get(), crl.get()) != 1)
            {
                throw std::runtime_error("Certificate revocation list is invalid, not yet current or expired");
            }
            ++count;
        }
        if (count == 0 ||
            X509_STORE_set_flags(store.get(), X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL) != 1)
        {
            throw std::runtime_error("Cannot load certificate revocation list");
        }
    }
    if (X509_STORE_CTX_init(context.get(), store.get(), leaf.get(), chain.get()) != 1 ||
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_CLIENT) != 1)
    {
        throw std::runtime_error("Cannot initialize client certificate verification");
    }
    X509_VERIFY_PARAM_set_depth(X509_STORE_CTX_get0_param(context.get()), verifyDepth);
    if (X509_verify_cert(context.get()) != 1)
    {
        throw std::runtime_error(std::string("Client certificate refused: ") +
                                 X509_verify_cert_error_string(X509_STORE_CTX_get_error(context.get())));
    }
    if (!identityField.empty() && identityField != "off" &&
        (username.empty() || identity(leaf.get(), identityField) != username))
    {
        throw std::runtime_error("MQTT username does not match certificate identity");
    }
}
} // namespace xmq
