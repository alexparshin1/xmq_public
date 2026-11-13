/* Copyright (c) 2026 Alexey Parshin. MPL-2.0. */
// Standalone harness: drives the same class and host ABI as the shared library, without a broker.
#include "../Jwt.cpp"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

std::map<std::string, std::string> settings;

void logMessage(void*, xmq_log_priority, const char* message)
{
    std::cerr << message << '\n';
}

int64_t getSetting(void*, const char* key, char* buffer, size_t capacity)
{
    const auto found = settings.find(key);
    if (found == settings.end())
    {
        return -1;
    }
    const auto size = found->second.size() + 1;
    if (buffer && capacity >= size)
    {
        std::memcpy(buffer, found->second.c_str(), size);
    }
    return static_cast<int64_t>(size - 1);
}

void expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string base64UrlEncode(const std::string_view bytes)
{
    std::string encoded;
    size_t      position = 0;
    while (position < bytes.size())
    {
        const auto remaining = bytes.size() - position;
        uint32_t   buffer = static_cast<unsigned char>(bytes[position]) << 16;
        if (remaining > 1) buffer |= static_cast<unsigned char>(bytes[position + 1]) << 8;
        if (remaining > 2) buffer |= static_cast<unsigned char>(bytes[position + 2]);
        encoded.push_back(base64Alphabet[(buffer >> 18) & 63]);
        encoded.push_back(base64Alphabet[(buffer >> 12) & 63]);
        if (remaining > 1) encoded.push_back(base64Alphabet[(buffer >> 6) & 63]);
        if (remaining > 2) encoded.push_back(base64Alphabet[buffer & 63]);
        position += 3;
    }
    return encoded;
}

std::string field(const std::string& name, const std::string& value)
{
    return "\"" + name + "\":\"" + value + "\"";
}

struct Signer
{
    std::string        algorithm;
    std::string        secret;   ///< HS256
    EVP_PKEY*          key {};   ///< RS256/ES256
};

std::string sign(const Signer& signer, const std::string& signingInput)
{
    if (signer.algorithm == "HS256")
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  length = 0;
        HMAC(EVP_sha256(), signer.secret.data(), static_cast<int>(signer.secret.size()),
             reinterpret_cast<const unsigned char*>(signingInput.data()), signingInput.size(),
             digest, &length);
        return std::string(reinterpret_cast<char*>(digest), length);
    }
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                          EVP_MD_CTX_free);
    EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, signer.key);
    size_t signatureSize = 0;
    EVP_DigestSign(context.get(), nullptr, &signatureSize,
                   reinterpret_cast<const unsigned char*>(signingInput.data()),
                   signingInput.size());
    std::string signature(signatureSize, '\0');
    EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()),
                   &signatureSize,
                   reinterpret_cast<const unsigned char*>(signingInput.data()),
                   signingInput.size());
    signature.resize(signatureSize);
    if (signer.algorithm != "ES256")
    {
        return signature;
    }
    // OpenSSL signs with DER; a JWT carries r||s.
    const unsigned char* derPosition = reinterpret_cast<const unsigned char*>(signature.data());
    ECDSA_SIG*           ecdsa = d2i_ECDSA_SIG(nullptr, &derPosition,
                                               static_cast<long>(signature.size()));
    if (ecdsa == nullptr)
    {
        throw std::runtime_error("ES256 signature could not be decoded");
    }
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(ecdsa, &r, &s);
    std::string raw(64, '\0');
    BN_bn2binpad(r, reinterpret_cast<unsigned char*>(raw.data()), 32);
    BN_bn2binpad(s, reinterpret_cast<unsigned char*>(raw.data() + 32), 32);
    ECDSA_SIG_free(ecdsa);
    return raw;
}

std::string makeToken(const std::string& header, const std::string& claims, const Signer& signer)
{
    const auto signingInput = base64UrlEncode(header) + "." + base64UrlEncode(claims);
    return signingInput + "." + base64UrlEncode(sign(signer, signingInput));
}

std::string claimsWith(const std::string& extra)
{
    const auto now = std::time(nullptr);
    return "{\"iss\":\"https://idp.example.org/realms/xmq\",\"aud\":\"xmq\","
           "\"preferred_username\":\"alice\",\"groups\":[\"Operators\"],"
           "\"exp\":" + std::to_string(now + 300) + ",\"nbf\":" + std::to_string(now - 10) +
           (extra.empty() ? "" : "," + extra) + "}";
}

/// A JWKS document holding one RSA public key, as a provider would publish it.
std::string jwksFor(EVP_PKEY* key, const std::string& keyId)
{
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n);
    EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e);
    std::string modulus(BN_num_bytes(n), '\0');
    std::string exponent(BN_num_bytes(e), '\0');
    BN_bn2bin(n, reinterpret_cast<unsigned char*>(modulus.data()));
    BN_bn2bin(e, reinterpret_cast<unsigned char*>(exponent.data()));
    BN_free(n);
    BN_free(e);
    return "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"" + keyId +
           "\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"" +
           base64UrlEncode(modulus) + "\",\"e\":\"" + base64UrlEncode(exponent) + "\"}]}";
}

EVP_PKEY* generateRsa()
{
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY_keygen_init(context);
    EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(context, &key);
    EVP_PKEY_CTX_free(context);
    return key;
}

EVP_PKEY* generateEc()
{
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY_keygen_init(context);
    EVP_PKEY_CTX_set_group_name(context, "prime256v1");
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(context, &key);
    EVP_PKEY_CTX_free(context);
    return key;
}

std::string publicPem(EVP_PKEY* key)
{
    BIO* raw = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(raw, key);
    char*      data = nullptr;
    const auto length = BIO_get_mem_data(raw, &data);
    const std::string pem(data, static_cast<size_t>(length));
    BIO_free(raw);
    return pem;
}

xmq_auth_decision auth(const std::string& username, const std::string& password)
{
    xmq_host host {};
    host.struct_size = sizeof(host);
    host.setting = getSetting;
    host.log = logMessage;
    JwtAuthenticator extension(host);
    if (!extension.start())
    {
        throw std::runtime_error("the settings were not accepted");
    }
    xmq_auth_request request {};
    request.username = {username.data(), username.size()};
    request.password = {password.data(), password.size()};
    return extension.authenticate(request);
}

xmq_auth_decision auth(JwtAuthenticator& extension, const std::string& username,
                       const std::string& password)
{
    xmq_auth_request request {};
    request.username = {username.data(), username.size()};
    request.password = {password.data(), password.size()};
    return extension.authenticate(request);
}

int64_t epoch(const int offset)
{
    return static_cast<int64_t>(std::time(nullptr)) + offset;
}

} // namespace

int main()
{
    try
    {
        // base64url: the URL-safe alphabet, no padding, and a padding-bit check that refuses a
        // re-encoded value a signer never produced.
        {
            expect(base64UrlDecode("SGVsbG8") == std::string("Hello"), "base64url without padding");
            expect(base64UrlDecode("aGVsbG8td29ybGQ") == std::string("hello-world"),
                   "base64url alphabet");
            expect(!base64UrlDecode("a+b/").has_value(), "base64url refuses the padded alphabet");
            expect(base64UrlDecode("AA") == std::string(1, '\0'), "zero bytes survive");
        }

        // JSON: nested objects, arrays, escapes, numbers; and refusal rather than a half answer.
        {
            const auto document = parseJson(
                "{\"a\":{\"b\":[1,\"two\",true,null]},\"c\":\"x\\u00e9y\",\"n\":-12.5}");
            expect(document.has_value(), "json parses");
            expect(document->stringAt("a.b") == "", "an array is not a string");
            expect(document->stringsAt("a.b") == std::vector<std::string> {"two"},
                   "strings come out of an array");
            expect(document->stringAt("c") == std::string("x\xc3\xa9y"), "an escaped code point");
            expect(document->find("n")->number == -12.5, "a number keeps its value and sign");
            expect(!parseJson("{\"a\":1").has_value(), "an unfinished object is refused");
            expect(!parseJson("[] trailing").has_value(), "trailing text is refused");
            expect(!parseJson(std::string(64, '[')).has_value(), "a nesting bomb is refused");
        }

        // A token that is not three base64url parts is not ours: the chain keeps its local accounts.
        {
            expect(!looksLikeToken("hunter2"), "a password is not a token");
            expect(!looksLikeToken("a.b"), "two parts are not a token");
            expect(!looksLikeToken("a.b.c.d"), "four parts are not a token");
            expect(looksLikeToken("a.b.c"), "three base64url parts look like a token");
        }

        // HS256 through the extension itself: the identity must match the token, and a tampered
        // token, another secret, an expired token or another issuer are all refused.
        {
            settings = {{"algorithm", "HS256"}, {"secret", "test-secret"},
                        {"issuer", "https://idp.example.org/realms/xmq"},
                        {"audience", "xmq"}, {"groups_claim", "groups"}};
            const Signer signer {.algorithm = "HS256", .secret = "test-secret"};
            const auto   header = R"({"alg":"HS256","typ":"JWT"})";
            const auto   token = makeToken(header, claimsWith(""), signer);
            expect(auth("jwt/alice", token) == XMQ_AUTH_ALLOW, "a good HS256 token is admitted");

            xmq_host host {};
            host.struct_size = sizeof(host);
            host.setting = getSetting;
            host.log = logMessage;
            JwtAuthenticator extension(host);
            expect(extension.start(), "HS256 settings accepted");
            expect(auth(extension, "jwt/alice", token) == XMQ_AUTH_ALLOW, "and admitted again");
            expect(auth(extension, "jwt/bob", token) == XMQ_AUTH_DENY,
                   "a token for alice presented as bob is refused");

            auto tampered = token;
            tampered.back() = tampered.back() == 'A' ? 'B' : 'A';
            expect(auth(extension, "jwt/alice", tampered) == XMQ_AUTH_DENY,
                   "a tampered signature is refused");

            const Signer other {.algorithm = "HS256", .secret = "another-secret"};
            const auto   foreign = makeToken(header, claimsWith(""), other);
            expect(auth(extension, "jwt/alice", foreign) == XMQ_AUTH_DENY,
                   "a token signed with another secret is refused");

            const auto  expired = makeToken(header, claimsWith(field("iss", "https://idp.example.org/realms/xmq")), signer);
            expect(auth(extension, "jwt/alice", expired) == XMQ_AUTH_ALLOW, "precondition: valid");

            const auto expiredToken = makeToken(
                header,
                "{\"iss\":\"https://idp.example.org/realms/xmq\",\"aud\":\"xmq\","
                "\"preferred_username\":\"alice\",\"exp\":" + std::to_string(epoch(-600)) + "}",
                signer);
            expect(auth(extension, "jwt/alice", expiredToken) == XMQ_AUTH_DENY,
                   "an expired token is refused");

            const auto noExpiry = makeToken(
                header,
                "{\"iss\":\"https://idp.example.org/realms/xmq\",\"aud\":\"xmq\","
                "\"preferred_username\":\"alice\"}",
                signer);
            expect(auth(extension, "jwt/alice", noExpiry) == XMQ_AUTH_DENY,
                   "a token without an expiry is refused");

            const auto futureToken = makeToken(
                header,
                "{\"iss\":\"https://idp.example.org/realms/xmq\",\"aud\":\"xmq\","
                "\"preferred_username\":\"alice\",\"exp\":" + std::to_string(epoch(300)) +
                    ",\"nbf\":" + std::to_string(epoch(3600)) + "}",
                signer);
            expect(auth(extension, "jwt/alice", futureToken) == XMQ_AUTH_DENY,
                   "a token that is not valid yet is refused");

            const auto wrongIssuer = makeToken(
                header,
                "{\"iss\":\"https://other.example.org\",\"aud\":\"xmq\","
                "\"preferred_username\":\"alice\",\"exp\":" + std::to_string(epoch(300)) + "}",
                signer);
            expect(auth(extension, "jwt/alice", wrongIssuer) == XMQ_AUTH_DENY,
                   "another issuer is refused");

            const auto wrongAudience = makeToken(
                header,
                "{\"iss\":\"https://idp.example.org/realms/xmq\",\"aud\":\"other\","
                "\"preferred_username\":\"alice\",\"exp\":" + std::to_string(epoch(300)) + "}",
                signer);
            expect(auth(extension, "jwt/alice", wrongAudience) == XMQ_AUTH_DENY,
                   "another audience is refused");

            // A token asking for an algorithm the extension does not use is refused before the
            // key matters: this is the RS256-to-HS256 substitution.
            const auto wrongAlgorithm = makeToken(R"({"alg":"HS512"})", claimsWith(""), signer);
            expect(auth(extension, "jwt/alice", wrongAlgorithm) == XMQ_AUTH_DENY,
                   "a token chooses neither the algorithm nor the key");
        }

        // RS256 and ES256 with a configured public key.
        {
            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> rsa(generateRsa(), EVP_PKEY_free);
            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ec(generateEc(), EVP_PKEY_free);
            for (const auto& [algorithm, key]:
                 std::vector<std::pair<std::string, EVP_PKEY*>> {{"RS256", rsa.get()},
                                                                 {"ES256", ec.get()}})
            {
                settings = {{"algorithm", algorithm}, {"public_key", publicPem(key)},
                            {"issuer", "https://idp.example.org/realms/xmq"},
                            {"audience", "xmq"}};
                const Signer signer {.algorithm = algorithm, .key = key};
                const auto   header = "{\"alg\":\"" + algorithm + "\",\"typ\":\"JWT\"}";
                const auto   token = makeToken(header, claimsWith(""), signer);
                expect(auth("jwt/alice", token) == XMQ_AUTH_ALLOW,
                       (algorithm + " token is admitted").c_str());

                // Signed by the other key of the same kind: the signature, not the header, decides.
                const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> otherKey(
                    algorithm == "RS256" ? generateRsa() : generateEc(), EVP_PKEY_free);
                const Signer other {.algorithm = algorithm, .key = otherKey.get()};
                const auto   foreign = makeToken(header, claimsWith(""), other);
                expect(auth("jwt/alice", foreign) == XMQ_AUTH_DENY,
                       "a signature from another key is refused");
            }
        }

        // A JWKS document: keys are matched by kid and algorithm, and an unusable set is an error
        // rather than an empty answer.
        {
            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> rsa(generateRsa(), EVP_PKEY_free);
            const auto document = parseJson(jwksFor(rsa.get(), "k1"));
            expect(document.has_value(), "a JWKS document parses");
            const auto keys = keysFromJwks(*document);
            expect(keys.size() == 1 && keys[0].keyId == "k1" && keys[0].key,
                   "an RSA JWK becomes a key");
            const Signer signer {.algorithm = "RS256", .key = rsa.get()};
            const auto   token = makeToken(R"({"alg":"RS256","kid":"k1"})", claimsWith(""), signer);
            const auto   parsed = splitToken(token);
            expect(parsed.has_value(), "the token splits");
            expect(verifyRs256(keys[0].key.get(), parsed->signingInput, parsed->signature),
                   "a token verifies against the JWK");
            expect(!keysFromJwks(*parseJson("{\"keys\":[]}")).size(), "an empty JWKS holds nothing");
        }

        // EC JWK import uses provider parameters too: the raw point must still be P-256 and on
        // the curve. Checking only the coordinate lengths would accept attacker-supplied garbage.
        {
            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ec(generateEc(), EVP_PKEY_free);
            std::array<unsigned char, 65> point {};
            size_t length = 0;
            expect(EVP_PKEY_get_octet_string_param(ec.get(), OSSL_PKEY_PARAM_PUB_KEY,
                                                   point.data(), point.size(), &length) == 1 &&
                       length == point.size() && point[0] == 4,
                   "an EC provider exports an uncompressed P-256 point");
            const auto x = base64UrlEncode(std::string_view(reinterpret_cast<const char*>(point.data() + 1), 32));
            const auto y = base64UrlEncode(std::string_view(reinterpret_cast<const char*>(point.data() + 33), 32));
            const auto key = ecKeyFromJwk("P-256", x, y);
            expect(static_cast<bool>(key), "a P-256 JWK imports through the provider");
            const Signer signer {.algorithm = "ES256", .key = ec.get()};
            const auto token = splitToken(makeToken(R"({"alg":"ES256"})", claimsWith(""), signer));
            expect(token && verifyEs256(key.get(), token->signingInput, token->signature),
                   "the imported EC JWK verifies ES256");
            expect(!ecKeyFromJwk("P-384", x, y), "another curve is refused");
            expect(!ecKeyFromJwk("P-256", "AA", y), "a short coordinate is refused");
            const auto zero = base64UrlEncode(std::string(32, '\0'));
            expect(!ecKeyFromJwk("P-256", zero, zero), "a point outside the curve is refused");
            expect(!rsaKeyFromJwk("", "AQAB"), "an absent RSA modulus is refused");
            expect(!rsaKeyFromJwk("AA", "AA"), "invalid RSA parameters are refused");
        }

        // The decision cache: lifetimes both ways, an outage never stored, and reload clearing it.
        {
            DecisionCache cache;
            xmq_auth_decision decision {};
            std::vector<std::string> groups;
            cache.configure(60, 10, 4096);
            cache.store("allow", XMQ_AUTH_ALLOW, {"Operators"});
            expect(cache.find("allow", decision, groups) && decision == XMQ_AUTH_ALLOW &&
                   groups.size() == 1, "an allow is remembered with its groups");
            cache.store("deny", XMQ_AUTH_DENY, {});
            expect(cache.find("deny", decision, groups) && decision == XMQ_AUTH_DENY,
                   "a refusal is remembered");
            cache.store("outage", XMQ_AUTH_SUBSYSTEM_ERROR, {});
            expect(!cache.find("outage", decision, groups), "an outage is never remembered");
            cache.configure(0, 10, 4096);
            cache.store("fresh", XMQ_AUTH_ALLOW, {});
            expect(!cache.find("fresh", decision, groups), "zero positive lifetime caches nothing");
            cache.configure(1, 1, 4096);
            cache.store("young", XMQ_AUTH_ALLOW, {});
            expect(cache.find("young", decision, groups), "precondition: young entry hits");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            expect(!cache.find("young", decision, groups), "an expired entry misses");
            cache.configure(60, 10, 2);
            cache.store("x", XMQ_AUTH_ALLOW, {});
            cache.store("y", XMQ_AUTH_ALLOW, {});
            expect(cache.find("x", decision, groups), "precondition: both present");
            cache.store("z", XMQ_AUTH_ALLOW, {});
            expect(cache.find("x", decision, groups) && !cache.find("y", decision, groups) &&
                   cache.find("z", decision, groups), "the least recently used entry goes first");
        }

        // A keyed cache key: never the token itself, and a restart shares no keys.
        {
            std::array<unsigned char, 32> salt {};
            salt.fill(9);
            const auto key = decisionKey(salt, "jwt/alice", "header.payload.signature");
            expect(!key.empty() &&
                       key == decisionKey(salt, "jwt/alice", "header.payload.signature"),
                   "the same token gives the same key");
            expect(key != decisionKey(salt, "jwt/alice", "header.payload.other"),
                   "another token, another key");
            expect(key != decisionKey(salt, "jwt/bob", "header.payload.signature"),
                   "another identity, another key");
            expect(key.find("payload") == std::string::npos, "the key does not carry the token");
            expect(decisionKey(std::array<unsigned char, 32> {}, "jwt/alice", "token").empty(),
                   "without entropy nothing is cached");
        }

#ifndef _WIN32
        // The key set over the network, which is how a provider rotates its keys. A one-shot HTTP
        // server on loopback is the documented exception to the https rule.
        {
            const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> rsa(generateRsa(), EVP_PKEY_free);
            const auto body = jwksFor(rsa.get(), "k1");

            const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
            expect(listener >= 0, "a loopback listener can be opened");
            const int reuse = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            expect(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                   "the loopback listener binds");
            socklen_t addressSize = sizeof(address);
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize);
            expect(::listen(listener, 4) == 0, "the loopback listener listens");
            const auto port = ntohs(address.sin_port);

            std::thread server([listener, body]
            {
                // One fetch: start() is what asks for the set, and the cache answers the rest.
                for (int request = 0; request < 1; ++request)
                {
                    const int client = ::accept(listener, nullptr, nullptr);
                    if (client < 0)
                    {
                        return;
                    }
                    std::array<char, 2048> requestText {};
                    ::read(client, requestText.data(), requestText.size());
                    const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                               "Content-Length: " + std::to_string(body.size()) +
                                               "\r\nConnection: close\r\n\r\n" + body;
                    ::write(client, answer.data(), answer.size());
                    ::close(client);
                }
            });

            settings = {{"algorithm", "RS256"},
                        {"jwks_url", "http://127.0.0.1:" + std::to_string(port) + "/jwks"},
                        {"issuer", "https://idp.example.org/realms/xmq"},
                        {"audience", "xmq"}};
            const Signer signer {.algorithm = "RS256", .key = rsa.get()};
            const auto token = makeToken(R"({"alg":"RS256","kid":"k1"})", claimsWith(""), signer);

            xmq_host host {};
            host.struct_size = sizeof(host);
            host.setting = getSetting;
            host.log = logMessage;
            JwtAuthenticator extension(host);
            expect(extension.start(), "the key set is fetched at load time");
            expect(auth(extension, "jwt/alice", token) == XMQ_AUTH_ALLOW,
                   "a token is verified against a fetched key set");
            extension.stop();
            server.join();
            ::close(listener);
        }
#endif

        std::cout << "JWT tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "JWT test failed: " << error.what() << '\n';
        return 1;
    }
}
