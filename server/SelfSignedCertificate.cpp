/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "SelfSignedCertificate.h"

#include "common/HostName.h"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <array>
#include <memory>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr auto   keyBits = 2048;
constexpr auto   validYears = 10;
constexpr auto   serialBits = 64;
constexpr size_t maxHostNameLength = 256;

// Written through the OpenSSL API rather than by running the openssl command: the command is not
// there to be relied on - least of all on Windows - and the library is already linked in for TLS.
using KeyHandle = unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContextHandle = unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CertificateHandle = unique_ptr<X509, decltype(&X509_free)>;
using BioHandle = unique_ptr<BIO, decltype(&BIO_free_all)>;

// Files are opened by OpenSSL through a BIO rather than by fopen here, and the PEM_*_bio functions
// used instead of the FILE* ones. On Windows the library is built against its own C runtime, so a
// FILE* from this one means nothing to it: it routes such calls through an "uplink" table that
// only exists if the application compiles applink.c into its main module, and aborts the process
// when it does not. That would make every executable linking this code - the server, the tests -
// responsible for a workaround, so the FILE* API is avoided altogether.

/**
 * @brief The reason OpenSSL last refused something, in a form worth logging.
 */
String opensslError()
{
    constexpr size_t           messageLength = 256;
    array<char, messageLength> message{};
    if (const auto error = ERR_get_error();
        error != 0)
    {
        ERR_error_string_n(error, message.data(), message.size());
        return {message.data()};
    }
    return "unknown error";
}

/**
 * @brief The names a browser may reach this server under.
 *
 * A certificate is accepted for the name in the address bar, not for the machine, so the names it
 * can be reached by locally are all listed: the host's own name, its short form, and loopback.
 * Reaching it by any other name - a LAN address, an alias, a tunnel - shows a name mismatch on top
 * of the untrusted-issuer warning, which is the point at which a real certificate is worth having.
 */
Strings subjectAlternativeNames(const String& hostName)
{
    Strings names;

    const auto addName = [&names](const String& name)
    {
        if (const auto entry = "DNS:" + name;
            !name.empty() && ranges::find(names, entry) == names.end())
        {
            names.push_back(entry);
        }
    };

    addName(hostName);
    if (const auto dot = hostName.find('.');
        dot != String::npos)
    {
        addName(hostName.substr(0, dot));
    }

    // This machine's own name as well as the one asked for. A configuration that has never been
    // through the setup page names the node "localhost", because that is what the shipped template
    // says, and a certificate that then knows nothing of the machine's real name fails for every
    // client that connects by it. Listing both costs nothing and is right either way.
    addName(thisHostName());
    if (const auto dot = thisHostName().find('.');
        dot != String::npos)
    {
        addName(thisHostName().substr(0, dot));
    }

    addName("localhost");
    names.push_back("IP:127.0.0.1");
    names.push_back("IP:::1");
    return names;
}

/**
 * @brief Add a name component to the certificate's subject.
 */
void addSubjectEntry(X509_NAME* subject, const char* field, const String& value)
{
    if (X509_NAME_add_entry_by_txt(subject, field, MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(value.c_str()), -1, -1, 0) != 1)
    {
        throw Exception("Can't set certificate " + String(field) + ": " + opensslError());
    }
}

/**
 * @brief Add an X509v3 extension to a certificate that is its own issuer.
 */
void addExtension(X509* certificate, const int extensionId, const String& value)
{
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    // Self-signed, so the issuer and the subject are the same certificate.
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);

    auto* extension = X509V3_EXT_conf_nid(nullptr, &context, extensionId, value.c_str());
    if (extension == nullptr)
    {
        throw Exception("Can't build certificate extension: " + opensslError());
    }

    const auto added = X509_add_ext(certificate, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    if (!added)
    {
        throw Exception("Can't add certificate extension: " + opensslError());
    }
}

/**
 * @brief Generate an RSA key pair.
 *
 * Through the generic EVP interface rather than EVP_RSA_gen(), which OpenSSL 1.1 does not have and
 * some of the platforms this is built on still carry.
 */
KeyHandle generateKey()
{
    const KeyContextHandle context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), keyBits) != 1)
    {
        throw Exception("Can't prepare a key: " + opensslError());
    }

    EVP_PKEY* generated = nullptr;
    if (EVP_PKEY_keygen(context.get(), &generated) != 1)
    {
        throw Exception("Can't generate a key: " + opensslError());
    }

    return {generated, &EVP_PKEY_free};
}

/**
 * @brief Build a self-signed certificate for the given key.
 */
CertificateHandle makeCertificate(EVP_PKEY* key, const String& hostName)
{
    CertificateHandle certificate(X509_new(), &X509_free);
    if (!certificate)
    {
        throw Exception("Can't create a certificate: " + opensslError());
    }

    // Version 3, which is what carries the subject alternative names; the field holds one less
    // than the version number.
    X509_set_version(certificate.get(), 2);

    // Random rather than sequential: a certificate reissued for the same name should not collide
    // with the one a browser remembers.
    if (const unique_ptr<BIGNUM, decltype(&BN_free)> serial(BN_new(), &BN_free);
        !serial || BN_rand(serial.get(), serialBits, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1 ||
        BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())) == nullptr)
    {
        throw Exception("Can't set the certificate serial number: " + opensslError());
    }

    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                    static_cast<long>(validYears) * 365 * 24 * 60 * 60);

    auto* subject = X509_get_subject_name(certificate.get());
    addSubjectEntry(subject, "O", "XMQ");
    // The node rather than the interface: one pair now identifies this server to browsers on the
    // configuration port and to clients and peers on the MQTT+SSL ones.
    addSubjectEntry(subject, "OU", "XMQ node");
    addSubjectEntry(subject, "CN", hostName);
    // Self-signed: it vouches for itself.
    X509_set_issuer_name(certificate.get(), subject);

    if (X509_set_pubkey(certificate.get(), key) != 1)
    {
        throw Exception("Can't set the certificate key: " + opensslError());
    }

    addExtension(certificate.get(), NID_basic_constraints, "critical,CA:FALSE");
    addExtension(certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment");
    addExtension(certificate.get(), NID_ext_key_usage, "serverAuth");
    addExtension(certificate.get(), NID_subject_key_identifier, "hash");
    addExtension(certificate.get(), NID_subject_alt_name, subjectAlternativeNames(hostName).join(","));

    if (X509_sign(certificate.get(), key, EVP_sha256()) == 0)
    {
        throw Exception("Can't sign the certificate: " + opensslError());
    }

    return certificate;
}

/**
 * @brief When a certificate stops being accepted, as a date worth reading.
 *
 * A certificate that has run out is indistinguishable, from the browser's side, from one that was
 * never any good - so the date belongs next to the fingerprint rather than in a place someone has
 * to go and look.
 *
 * @param certificate       Certificate to read.
 * @return the expiry date, or "unknown" when it cannot be read.
 */
String expiryOf(const X509* certificate)
{
    const BioHandle bio(BIO_new(BIO_s_mem()), &BIO_free_all);
    if (!bio || ASN1_TIME_print(bio.get(), X509_get0_notAfter(certificate)) != 1)
    {
        return "unknown";
    }

    char*      text = nullptr;
    const auto length = BIO_get_mem_data(bio.get(), &text);
    if (length <= 0 || text == nullptr)
    {
        return "unknown";
    }

    return {text, static_cast<size_t>(length)};
}

/**
 * @brief Move a file out of the way, keeping it beside its replacement.
 *
 * Nothing here is ever deleted outright: the file being replaced may be the only copy of a
 * certificate somebody published a fingerprint for, and the mistake is usually noticed after
 * the fact.
 *
 * @param file              File about to be replaced.
 */
void keepPrevious(const filesystem::path& file)
{
    if (!filesystem::exists(file))
    {
        return;
    }

    auto backupPath = file;
    backupPath += ".old";
    filesystem::rename(file, backupPath);
}

/**
 * @brief Write the private key, readable by its owner only.
 */
void writePrivateKey(const filesystem::path& privateKeyFile, const EVP_PKEY* key)
{
    const BioHandle bio(BIO_new_file(privateKeyFile.string().c_str(), "wb"), &BIO_free_all);
    if (!bio)
    {
        throw Exception("Can't write " + privateKeyFile.string() + ": " + opensslError());
    }

    if (PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
    {
        throw Exception("Can't write the private key: " + opensslError());
    }
}

/**
 * @brief Take away every permission but the owner's.
 *
 * Done after writing rather than by creating the file with the right mode, because the umask a
 * service inherits is not something to rely on. A private key other accounts on the machine can
 * read is a private key in name only.
 */
void restrictToOwner(const filesystem::path& file)
{
    error_code errorCode;
    filesystem::permissions(file,
                            filesystem::perms::owner_read | filesystem::perms::owner_write,
                            filesystem::perm_options::replace, errorCode);
}

} // namespace

bool SelfSignedCertificate::create(const filesystem::path& certificateFile, const filesystem::path& privateKeyFile,
                                   const String&           hostName, String&                        description)
{
    error_code errorCode;
    const auto hasCertificate = filesystem::exists(certificateFile, errorCode);
    const auto hasPrivateKey = filesystem::exists(privateKeyFile, errorCode);

    if (hasCertificate && hasPrivateKey)
    {
        return false;
    }

    // Half a pair is somebody's certificate with a problem, not an installation waiting for one.
    // Replacing the half that is there would destroy the only copy of it.
    if (hasCertificate != hasPrivateKey)
    {
        throw Exception("Found " + (hasCertificate ? certificateFile : privateKeyFile).string() +
                        " but not " + (hasCertificate ? privateKeyFile : certificateFile).string() +
                        ". Remove the one that is left to have a new pair generated, or restore the "
                        "missing one.");
    }

    issue(certificateFile, privateKeyFile, hostName, description);
    return true;
}

void SelfSignedCertificate::reissue(const filesystem::path& certificateFile,
                                    const filesystem::path& privateKeyFile,
                                    const String&           hostName, String& description)
{
    // Kept rather than removed: the pair being replaced may be one the administrator published a
    // fingerprint for, or handed to another node, and setting a server up again is not a reason
    // to make that unrecoverable.
    keepPrevious(certificateFile);
    keepPrevious(privateKeyFile);

    issue(certificateFile, privateKeyFile, hostName, description);
}

void SelfSignedCertificate::issue(const filesystem::path& certificateFile, const filesystem::path& privateKeyFile,
                                  const String&           hostName, String&                        description)
{
    error_code errorCode;
    if (const auto directory = certificateFile.parent_path();
        !directory.empty() && !filesystem::exists(directory, errorCode))
    {
        filesystem::create_directories(directory, errorCode);
    }

    const auto key = generateKey();
    const auto certificate = makeCertificate(key.get(), hostName);

    {
        const BioHandle bio(BIO_new_file(certificateFile.string().c_str(), "wb"), &BIO_free_all);
        if (!bio)
        {
            throw Exception(format("Can't open file {} for writing: {}", certificateFile.string(),
                                   opensslError().c_str()));
        }
        if (PEM_write_bio_X509(bio.get(), certificate.get()) != 1)
        {
            throw Exception(format("Can't write file {}: {}", certificateFile.string(),
                                   opensslError().c_str()));
        }
    }

    try
    {
        writePrivateKey(privateKeyFile, key.get());
    }
    catch (const Exception&)
    {
        // The certificate without its key is the half-pair this function refuses to work with, so
        // it does not leave one behind.
        filesystem::remove(certificateFile, errorCode);
        throw;
    }

    restrictToOwner(privateKeyFile);

    description = format("self-signed certificate for {}, valid for {} years, written to {}",
                         hostName.c_str(), validYears, certificateFile.string());
}

String SelfSignedCertificate::describe(const filesystem::path& certificateFile)
{
    const BioHandle bio(BIO_new_file(certificateFile.string().c_str(), "rb"), &BIO_free_all);
    if (!bio)
    {
        return {};
    }

    const CertificateHandle certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    if (!certificate)
    {
        return {};
    }

    array<char, maxHostNameLength> subject{};
    X509_NAME_oneline(X509_get_subject_name(certificate.get()), subject.data(),
                      static_cast<int>(subject.size()));

    array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned                              digestLength = 0;
    String                                fingerprint;
    if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &digestLength) == 1)
    {
        constexpr array<char, 17> hexDigits{"0123456789ABCDEF"};
        for (unsigned index = 0; index < digestLength; ++index)
        {
            if (index > 0)
            {
                fingerprint += ":";
            }
            fingerprint += hexDigits.at(digest.at(index) >> 4U);
            fingerprint += hexDigits.at(digest.at(index) & 0x0FU);
        }
    }

    return String(subject.data()) + ", expires " + expiryOf(certificate.get()) +
           ", SHA-256 fingerprint " + fingerprint;
}
