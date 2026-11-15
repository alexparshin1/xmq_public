#include "server/CertificateIdentity.h"
#include "test/ServerTests_Suite.h"
#include "client/MqttClient.h"
#include "common/SubscribeAckMessage.h"
#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

Key makeKey()
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* key = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
        EVP_PKEY_keygen(context.get(), &key) <= 0)
    {
        throw std::runtime_error("Cannot create test key");
    }
    return Key(key, EVP_PKEY_free);
}

std::string pem(X509* certificate)
{
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    if (!output || PEM_write_bio_X509(output.get(), certificate) != 1)
    {
        throw std::runtime_error("Cannot encode test certificate");
    }
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    return {data, static_cast<size_t>(size)};
}

class CertificateIdentityTests : public xmq::ServerTests_Suite
{
protected:
    Key key = makeKey();
    Certificate ca {nullptr, X509_free};
    std::filesystem::path directory;

    Certificate certificate(const char* cn, const long serial, const bool authority = false,
                            const char* san = nullptr, const long expires = 3600)
    {
        Certificate result(X509_new(), X509_free);
        if (!result)
        {
            throw std::runtime_error("Cannot create test certificate");
        }
        X509_set_version(result.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(result.get()), serial);
        X509_gmtime_adj(X509_getm_notBefore(result.get()), -7200);
        X509_gmtime_adj(X509_getm_notAfter(result.get()), expires);
        X509_set_pubkey(result.get(), key.get());
        const auto name = X509_get_subject_name(result.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
        X509_set_issuer_name(result.get(), authority ? name : X509_get_subject_name(ca.get()));
        X509V3_CTX context;
        X509V3_set_ctx(&context, authority ? result.get() : ca.get(), result.get(), nullptr, nullptr, 0);
        const auto add = [&](int nid, const char* value)
        {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
            if (!extension || X509_add_ext(result.get(), extension.get(), -1) != 1)
            {
                throw std::runtime_error("Cannot create test certificate extension");
            }
        };
        add(NID_basic_constraints, authority ? "critical,CA:TRUE" : "critical,CA:FALSE");
        add(NID_key_usage, authority ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
        if (!authority)
        {
            add(NID_ext_key_usage, "clientAuth");
        }
        if (san)
        {
            add(NID_subject_alt_name, san);
        }
        if (X509_sign(result.get(), key.get(), EVP_sha256()) <= 0)
        {
            throw std::runtime_error("Cannot sign test certificate");
        }
        return result;
    }

    void SetUp() override
    {
        directory = std::filesystem::temp_directory_path() /
                    ("xmq-certificate-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        ca = certificate("Test CA", 1, true);
        std::ofstream(directory / "ca.pem") << pem(ca.get());
    }

    void TearDown() override
    {
        stopServer("certificate-policy");
        std::error_code error;
        // Only the two files this fixture creates; never recursively remove a temporary tree.
        std::filesystem::remove(directory / "ca.pem", error);
        std::filesystem::remove(directory / "crl.pem", error);
        std::filesystem::remove(directory / "key.pem", error);
        std::filesystem::remove(directory / "client.pem", error);
        std::filesystem::remove(directory, error);
    }

    xmq::SServer startPolicyServer(const bool withCrl = false)
    {
        {
            std::unique_ptr<BIO, decltype(&BIO_free)> output(
                BIO_new_file((directory / "key.pem").string().c_str(), "w"), BIO_free);
            if (!output || PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
            {
                throw std::runtime_error("Cannot write fixture key");
            }
        }
        const auto leaf = certificate("user", 2);
        std::ofstream(directory / "client.pem") << pem(leaf.get());
        return createServer(1891, 8891, 9891, true, "certificate-policy", "user", "secret",
                            sptk::LogPriority::Warning, false, false,
                            [&](xmq::Settings& settings)
                            {
                                auto& keys = settings.m_connections.m_ssl_keys;
                                keys.m_cafile = (directory / "ca.pem").string();
                                keys.m_certfile = (directory / "ca.pem").string();
                                keys.m_keyfile = (directory / "key.pem").string();
                                keys.m_verify_depth = "3";
                                keys.m_identity_field = "cn";
                                keys.m_crlfile = withCrl ? (directory / "crl.pem").string() : "";
                            });
    }

    std::shared_ptr<sptk::SSLKeys> clientKeys()
    {
        return std::make_shared<sptk::SSLKeys>(directory / "key.pem", directory / "client.pem");
    }

    void crl(const long revokedSerial = 0, const long expires = 3600, const bool withNextUpdate = true)
    {
        std::unique_ptr<X509_CRL, decltype(&X509_CRL_free)> list(X509_CRL_new(), X509_CRL_free);
        ASSERT_TRUE(list);
        X509_CRL_set_version(list.get(), 1);
        X509_CRL_set_issuer_name(list.get(), X509_get_subject_name(ca.get()));
        std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> time(ASN1_TIME_new(), ASN1_TIME_free);
        ASSERT_TRUE(time);
        X509_gmtime_adj(time.get(), -7200);
        X509_CRL_set1_lastUpdate(list.get(), time.get());
        if (withNextUpdate)
        {
            X509_gmtime_adj(time.get(), expires);
            X509_CRL_set1_nextUpdate(list.get(), time.get());
        }
        if (revokedSerial)
        {
            const auto revoked = X509_REVOKED_new();
            ASSERT_NE(nullptr, revoked);
            std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)> serial(ASN1_INTEGER_new(), ASN1_INTEGER_free);
            ASN1_INTEGER_set(serial.get(), revokedSerial);
            X509_REVOKED_set_serialNumber(revoked, serial.get());
            X509_gmtime_adj(time.get(), -60);
            X509_REVOKED_set_revocationDate(revoked, time.get());
            ASSERT_EQ(1, X509_CRL_add0_revoked(list.get(), revoked));
        }
        X509_CRL_sort(list.get());
        ASSERT_GT(X509_CRL_sign(list.get(), key.get(), EVP_sha256()), 0);
        std::unique_ptr<BIO, decltype(&BIO_free)> file(
            BIO_new_file((directory / "crl.pem").string().c_str(), "w"), BIO_free);
        ASSERT_TRUE(file);
        ASSERT_EQ(1, PEM_write_bio_X509_CRL(file.get(), list.get()));
    }

    void verify(X509* leaf, const std::string& field = "cn", const std::string& username = "device",
                const bool withCrl = false)
    {
        xmq::verifyCertificateIdentity(pem(leaf), (directory / "ca.pem").string(),
                                       withCrl ? (directory / "crl.pem").string() : "",
                                       field, username, 3);
    }
};

TEST_F(CertificateIdentityTests, BindsCommonNameAndRefusesUsernameSubstitution)
{
    const auto leaf = certificate("device", 2);
    EXPECT_NO_THROW(verify(leaf.get()));
    EXPECT_THROW(verify(leaf.get(), "cn", "administrator"), std::runtime_error);
    EXPECT_THROW(verify(leaf.get(), "cn", ""), std::runtime_error);
}

TEST_F(CertificateIdentityTests, BindsSanWithoutFallingBackToCommonName)
{
    const auto leaf = certificate("device", 2, false, "URI:urn:device:one,email:device@example.test");
    EXPECT_NO_THROW(verify(leaf.get(), "san_uri", "urn:device:one"));
    EXPECT_NO_THROW(verify(leaf.get(), "san_email", "device@example.test"));
    EXPECT_THROW(verify(leaf.get(), "san_uri", "device"), std::runtime_error);
    EXPECT_THROW(verify(leaf.get(), "unknown"), std::runtime_error);
    const auto missing = certificate("device", 3);
    EXPECT_THROW(verify(missing.get(), "san_uri", "device"), std::runtime_error);
    const auto ambiguous = certificate("device", 4, false, "URI:urn:one,URI:urn:two");
    EXPECT_THROW(verify(ambiguous.get(), "san_uri", "urn:one"), std::runtime_error);
}

TEST_F(CertificateIdentityTests, RefusesExpiredAndUntrustedCertificates)
{
    const auto expired = certificate("device", 2, false, nullptr, -60);
    EXPECT_THROW(verify(expired.get()), std::runtime_error);
    const auto untrusted = certificate("device", 3, true);
    EXPECT_THROW(verify(untrusted.get()), std::runtime_error);
}

TEST_F(CertificateIdentityTests, RevocationIsCheckedAgainOnEveryConnect)
{
    const auto leaf = certificate("device", 2);
    crl();
    EXPECT_NO_THROW(verify(leaf.get(), "cn", "device", true));
    crl(2);
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
}

TEST_F(CertificateIdentityTests, MissingExpiredOrMalformedCrlFailsClosed)
{
    const auto leaf = certificate("device", 2);
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
    crl(0, -60);
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
    crl(0, 3600, false);
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
    std::ofstream(directory / "crl.pem") << "not a CRL";
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
}

TEST_F(CertificateIdentityTests, RejectsCrlWithInvalidIssuerSignature)
{
    const auto leaf = certificate("device", 2);
    auto issuerKey = std::move(key);
    key = makeKey();
    crl();
    key = std::move(issuerKey);
    EXPECT_THROW(verify(leaf.get(), "cn", "device", true), std::runtime_error);
}

TEST_F(CertificateIdentityTests, RejectsDuplicateCommonNamesAndServerOnlyPurpose)
{
    const auto leaf = certificate("device", 2);
    ASSERT_EQ(1, X509_NAME_add_entry_by_txt(X509_get_subject_name(leaf.get()), "CN", MBSTRING_ASC,
                                          reinterpret_cast<const unsigned char*>("other"), -1, -1, 0));
    ASSERT_GT(X509_sign(leaf.get(), key.get(), EVP_sha256()), 0);
    EXPECT_THROW(verify(leaf.get()), std::runtime_error);

    const auto serverOnly = certificate("device", 3);
    const auto index = X509_get_ext_by_NID(serverOnly.get(), NID_ext_key_usage, -1);
    ASSERT_GE(index, 0);
    X509_EXTENSION_free(X509_delete_ext(serverOnly.get(), index));
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_ext_key_usage, "serverAuth"), X509_EXTENSION_free);
    ASSERT_TRUE(extension);
    ASSERT_EQ(1, X509_add_ext(serverOnly.get(), extension.get(), -1));
    ASSERT_GT(X509_sign(serverOnly.get(), key.get(), EVP_sha256()), 0);
    EXPECT_THROW(verify(serverOnly.get()), std::runtime_error);
}

TEST_F(CertificateIdentityTests, CrlCheckingRequiresListsForIntermediateIssuersToo)
{
    const auto intermediate = certificate("Intermediate CA", 10, true);
    ASSERT_EQ(1, X509_set_issuer_name(intermediate.get(), X509_get_subject_name(ca.get())));
    ASSERT_GT(X509_sign(intermediate.get(), key.get(), EVP_sha256()), 0);
    const auto leaf = certificate("device", 2);
    ASSERT_EQ(1, X509_set_issuer_name(leaf.get(), X509_get_subject_name(intermediate.get())));
    ASSERT_GT(X509_sign(leaf.get(), key.get(), EVP_sha256()), 0);
    const auto chain = pem(leaf.get()) + pem(intermediate.get());
    EXPECT_NO_THROW(xmq::verifyCertificateIdentity(chain, (directory / "ca.pem").string(),
                                                   "", "cn", "device", 3));
    crl(); // Only the root issuer's CRL, no list issued by the intermediate.
    EXPECT_THROW(xmq::verifyCertificateIdentity(chain, (directory / "ca.pem").string(),
                                                (directory / "crl.pem").string(), "cn", "device", 3),
                 std::runtime_error);
}

TEST_F(CertificateIdentityTests, MissingCertificateOrTrustConfigurationFailsClosed)
{
    EXPECT_THROW(xmq::verifyCertificateIdentity("", "", "", "cn", "device", 0), std::runtime_error);
    const auto leaf = certificate("device", 2);
    EXPECT_THROW(xmq::verifyCertificateIdentity(pem(leaf.get()), (directory / "ca.pem").string(),
                                                "", "cn", "device", 0), std::runtime_error);
}

TEST_F(CertificateIdentityTests, MqttConnectBindsIdentityBeforePasswordAuthentication)
{
    const auto broker = startPolicyServer();
    for (const auto version : {xmq::ProtocolVersion::MqttV311, xmq::ProtocolVersion::MqttV5})
    {
        xmq::client::MqttClient client;
        EXPECT_EQ(xmq::ReasonCode::Success,
                  client.connect(sptk::Host("localhost", 8891), {"certificate-ok", "user", "secret"},
                                 {}, version, {}, clientKeys()));
        client.disconnect();
        EXPECT_NE(xmq::ReasonCode::Success,
                  client.connect(sptk::Host("localhost", 8891), {"certificate-forged", "admin", "admin"},
                                 {}, version, {}, clientKeys()))
            << "A valid password for another account must not override the certificate";
        EXPECT_NE(xmq::ReasonCode::Success,
                  client.connect(sptk::Host("localhost", 8891), {"certificate-password", "user", "wrong"},
                                 {}, version, {}, clientKeys()))
            << "Certificate binding must not silently disable existing password authentication";
    }
}

TEST_F(CertificateIdentityTests, MqttConnectCannotBypassPolicyUsingPlainTcpOrNoCertificate)
{
    const auto broker = startPolicyServer();
    xmq::client::MqttClient client;
    EXPECT_NE(xmq::ReasonCode::Success,
              client.connect(sptk::Host("localhost", 1891), {"certificate-plain", "user", "secret"},
                             {}, xmq::ProtocolVersion::MqttV5));
    EXPECT_NE(xmq::ReasonCode::Success,
              client.connect(sptk::Host("localhost", 8891), {"certificate-missing", "user", "secret"},
                             {}, xmq::ProtocolVersion::MqttV5, {}, std::make_shared<sptk::SSLKeys>()));
}

TEST_F(CertificateIdentityTests, MqttReconnectRefusesCertificateAfterCrlReplacement)
{
    crl();
    const auto broker = startPolicyServer(true);
    xmq::client::MqttClient client;
    EXPECT_EQ(xmq::ReasonCode::Success,
              client.connect(sptk::Host("localhost", 8891), {"certificate-revoked", "user", "secret"},
                             {}, xmq::ProtocolVersion::MqttV5, {}, clientKeys()));
    client.disconnect();
    crl(2);
    EXPECT_NE(xmq::ReasonCode::Success,
              client.connect(sptk::Host("localhost", 8891), {"certificate-revoked", "user", "secret"},
                             {}, xmq::ProtocolVersion::MqttV5, {}, clientKeys()));
}

TEST_F(CertificateIdentityTests, CertificateBoundIdentityUsesNativePublishAndSubscribePermissions)
{
    using namespace xmq;
    const auto broker = startPolicyServer();
    const auto store = broker->getSettings()->userManager().store();
    ASSERT_TRUE(store);
    store->replaceAclRules({
        {.m_groupName = "user:user", .m_action = "publish", .m_filter = "cert/%u/#",
         .m_source = "manual", .m_pattern = true},
        {.m_groupName = "user:user", .m_action = "subscribe", .m_filter = "cert/%u/#",
         .m_source = "manual", .m_pattern = true}
    });
    broker->reloadAclRules();

    sptk::Semaphore subscribed;
    sptk::Semaphore refused;
    sptk::Semaphore delivered;
    client::MqttClient client;
    client.onAck([&](const SMessage& message)
    {
        if (const auto ack = std::dynamic_pointer_cast<SubscribeAckMessage>(message);
            ack && !ack->subscriptionResults().empty())
        {
            if (ack->subscriptionResults().front() <= 2) subscribed.post();
            else refused.post();
        }
    });
    client.onMessage([&](const SPublishMessage&) { delivered.post(); });
    ASSERT_EQ(ReasonCode::Success,
              client.connect(sptk::Host("localhost", 8891), {"certificate-acl", "user", "secret"},
                             {}, ProtocolVersion::MqttV5, {}, clientKeys()));
    client.subscribe(Destination(client::MqttClient::getTopic("cert/user/#")));
    ASSERT_TRUE(subscribed.wait_for(std::chrono::seconds(5)));
    client.subscribe(Destination(client::MqttClient::getTopic("cert/admin/#")));
    ASSERT_TRUE(refused.wait_for(std::chrono::seconds(5)));

    const auto session = broker->getClientSession("certificate-acl");
    ASSERT_TRUE(session);
    EXPECT_EQ(ReasonCode::Success, Server::grantPublish(session, client::MqttClient::getTopic("cert/user/value")));
    EXPECT_NE(ReasonCode::Success, Server::grantPublish(session, client::MqttClient::getTopic("cert/admin/value")));
    client.publish("cert/user/value", "delivered");
    EXPECT_TRUE(delivered.wait_for(std::chrono::seconds(5)));
    client.disconnect();
}

TEST_F(CertificateIdentityTests, RefusedCertificateCannotTakeOverAnExistingSession)
{
    crl();
    const auto broker = startPolicyServer(true);
    xmq::client::MqttClient incumbent;
    ASSERT_EQ(xmq::ReasonCode::Success,
              incumbent.connect(sptk::Host("localhost", 8891), {"certificate-incumbent", "user", "secret"},
                                {}, xmq::ProtocolVersion::MqttV5, {}, clientKeys()));
    const auto session = broker->getClientSession("certificate-incumbent");
    ASSERT_TRUE(session);
    const auto originalSocket = session->getSocket();
    ASSERT_TRUE(originalSocket);
    crl(2);
    xmq::client::MqttClient replacement;
    EXPECT_NE(xmq::ReasonCode::Success,
              replacement.connect(sptk::Host("localhost", 8891), {"certificate-incumbent", "user", "secret"},
                                  {}, xmq::ProtocolVersion::MqttV5, {}, clientKeys()));
    EXPECT_EQ(session, broker->getClientSession("certificate-incumbent"));
    EXPECT_EQ(originalSocket, session->getSocket());
    EXPECT_TRUE(originalSocket->active());
    incumbent.disconnect();
}

TEST_F(CertificateIdentityTests, InvalidPolicySettingsCannotReplaceTheCurrentPolicy)
{
    const auto broker = startPolicyServer();
    const auto settings = broker->getSettings();
    auto keys = settings->sslKeysControl("get", xmq::CSSLKeys{});
    keys.m_identity_field = "first_available_name";
    EXPECT_THROW(settings->sslKeysControl("set", keys), sptk::Exception);
    keys.m_identity_field = "cn";
    keys.m_verify_depth = "0";
    EXPECT_THROW(settings->sslKeysControl("set", keys), sptk::Exception);
    EXPECT_EQ("cn", settings->sslKeysControl("get", xmq::CSSLKeys{}).m_identity_field.asString());
}

TEST_F(CertificateIdentityTests, DisabledPolicyPreservesPlainAndTlsPasswordAuthentication)
{
    const auto broker = startPolicyServer();
    auto keys = broker->getSettings()->sslKeysControl("get", xmq::CSSLKeys{});
    keys.m_identity_field = "off";
    broker->getSettings()->sslKeysControl("set", keys);
    for (const auto version : {xmq::ProtocolVersion::MqttV311, xmq::ProtocolVersion::MqttV5})
    {
        xmq::client::MqttClient client;
        EXPECT_EQ(xmq::ReasonCode::Success,
                  client.connect(sptk::Host("localhost", 1891), {"policy-off-plain", "user", "secret"},
                                 {}, version));
        client.disconnect();
        EXPECT_EQ(xmq::ReasonCode::Success,
                  client.connect(sptk::Host("localhost", 8891), {"policy-off-tls", "user", "secret"},
                                 {}, version, {}, std::make_shared<sptk::SSLKeys>()));
        client.disconnect();
    }
}
}
