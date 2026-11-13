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

/**
 * @file JwtVerify.h
 * @brief Everything a token check needs and nothing about the broker: base64url, JSON, and the
 *        three signature algorithms.
 *
 * Kept apart from the extension so it can be tested without a broker, a network or a configuration
 * file - a signature check is arithmetic, and arithmetic is what tests are for.
 *
 * The broker does not need a directory for this: the token carries a signature the extension can
 * check by itself, and the claims say who the client is and what it may be. That is the point of
 * the mode - a client that already holds a token from the customer's identity provider connects
 * without another round trip, and the broker trusts the issuer only as far as the signature and
 * the claims go.
 *
 * Three signature algorithms are accepted: HS256 with a shared secret, RS256 with an RSA public
 * key, and ES256 with a P-256 public key. Keys are either configured directly or fetched from the
 * issuer's JWKS endpoint, which is what lets a provider rotate its signing key without the broker
 * being reconfigured.
 *
 * The password is where the token goes, because MQTT has no field of its own for one. A password
 * that is not three dot-separated base64url parts is somebody else's business: the extension
 * answers NOT_HANDLED and the chain carries on, so local accounts keep working next to it.
 *
 * Claim names are configurable, and a claim may be a dotted path ("realm_access.roles"), because
 * providers disagree about where they put groups. A group claim may be a string or an array.
 */

#include "extension/XmqExtension.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// base64url, which is base64 with two characters changed and no padding.
// ---------------------------------------------------------------------------------------------

constexpr char base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int base64Value(const char symbol)
{
    if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
    if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
    if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
    if (symbol == '-') return 62;
    if (symbol == '_') return 63;
    return -1;
}

/// Decodes one base64url part. `nullopt` rather than a partial answer: half a token must not be
/// mistaken for a shortened one.
std::optional<std::string> base64UrlDecode(const std::string_view text)
{
    std::string decoded;
    decoded.reserve(text.size() * 3 / 4 + 3);
    uint32_t buffer = 0;
    int      bits = 0;
    for (const char symbol: text)
    {
        const auto value = base64Value(symbol);
        if (value < 0)
        {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    // A leftover 6-bit group carries padding bits that must be zero, or the encoding is not one a
    // signer produced.
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0)
    {
        return std::nullopt;
    }
    return decoded;
}

// ---------------------------------------------------------------------------------------------
// JSON, as much of it as a token and a key set need.
// ---------------------------------------------------------------------------------------------

struct Json
{
    enum class Type { Null, Boolean, Number, String, Array, Object } type {Type::Null};
    bool                                   boolean {false};
    double                                 number {0};
    std::string                            text;
    std::vector<Json>                      items;
    std::map<std::string, Json, std::less<>> fields;

    const Json* find(const std::string_view name) const
    {
        const auto found = fields.find(name);
        return found == fields.end() ? nullptr : &found->second;
    }

    /// The string at a dotted path, or an empty string. A missing middle simply means absent.
    std::string stringAt(const std::string_view path) const
    {
        const Json* node = this;
        size_t      start = 0;
        while (start <= path.size())
        {
            const auto end = path.find('.', start);
            const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start
                                                                              : end - start);
            if (node->type != Type::Object)
            {
                return {};
            }
            node = node->find(part);
            if (node == nullptr)
            {
                return {};
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        if (node->type == Type::String)
        {
            return node->text;
        }
        if (node->type == Type::Number)
        {
            std::ostringstream textOut;
            textOut << node->number;
            return textOut.str();
        }
        return {};
    }

    /// The values at a dotted path: an array as it stands, a single string as one item.
    std::vector<std::string> stringsAt(const std::string_view path) const
    {
        const Json* node = this;
        size_t      start = 0;
        while (start <= path.size())
        {
            const auto end = path.find('.', start);
            const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start
                                                                              : end - start);
            if (node->type != Type::Object)
            {
                return {};
            }
            node = node->find(part);
            if (node == nullptr)
            {
                return {};
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        std::vector<std::string> values;
        if (node->type == Type::Array)
        {
            for (const auto& item: node->items)
            {
                if (item.type == Type::String)
                {
                    values.push_back(item.text);
                }
            }
        }
        else if (node->type == Type::String)
        {
            values.push_back(node->text);
        }
        return values;
    }
};

/// A parser with boundaries. Tokens arrive from the network, so neither depth nor size may be
/// whatever the sender felt like: a nesting bomb is a stack overflow, not a parse error.
class JsonParser
{
public:
    explicit JsonParser(const std::string_view text)
        : m_text(text)
    {
    }

    std::optional<Json> parse()
    {
        auto value = parseValue(0);
        skipSpace();
        if (!value || m_position != m_text.size())
        {
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr size_t maxDepth = 32;

    void skipSpace()
    {
        while (m_position < m_text.size() &&
               (m_text[m_position] == ' ' || m_text[m_position] == '\t' ||
                m_text[m_position] == '\n' || m_text[m_position] == '\r'))
        {
            ++m_position;
        }
    }

    bool consume(const std::string_view word)
    {
        if (m_text.substr(m_position, word.size()) != word)
        {
            return false;
        }
        m_position += word.size();
        return true;
    }

    std::optional<Json> parseValue(const size_t depth)
    {
        if (depth > maxDepth)
        {
            return std::nullopt;
        }
        skipSpace();
        if (m_position >= m_text.size())
        {
            return std::nullopt;
        }
        Json value;
        switch (m_text[m_position])
        {
            case 'n':
                if (!consume("null")) return std::nullopt;
                value.type = Json::Type::Null;
                return value;
            case 't':
                if (!consume("true")) return std::nullopt;
                value.type = Json::Type::Boolean;
                value.boolean = true;
                return value;
            case 'f':
                if (!consume("false")) return std::nullopt;
                value.type = Json::Type::Boolean;
                return value;
            case '"':
                if (auto text = parseString(); text) { value.type = Json::Type::String; value.text = std::move(*text); return value; }
                return std::nullopt;
            case '[':
                return parseArray(depth);
            case '{':
                return parseObject(depth);
            default:
                break;
        }
        if (m_text[m_position] == '-' || (m_text[m_position] >= '0' && m_text[m_position] <= '9'))
        {
            return parseNumber();
        }
        return std::nullopt;
    }

    std::optional<std::string> parseString()
    {
        if (m_text[m_position] != '"')
        {
            return std::nullopt;
        }
        ++m_position;
        std::string text;
        while (m_position < m_text.size())
        {
            const auto symbol = m_text[m_position++];
            if (symbol == '"')
            {
                return text;
            }
            if (symbol != '\\')
            {
                text.push_back(symbol);
                continue;
            }
            if (m_position >= m_text.size())
            {
                return std::nullopt;
            }
            const auto escaped = m_text[m_position++];
            switch (escaped)
            {
                case '"': text.push_back('"'); break;
                case '\\': text.push_back('\\'); break;
                case '/': text.push_back('/'); break;
                case 'b': text.push_back('\b'); break;
                case 'f': text.push_back('\f'); break;
                case 'n': text.push_back('\n'); break;
                case 'r': text.push_back('\r'); break;
                case 't': text.push_back('\t'); break;
                case 'u':
                {
                    if (m_position + 4 > m_text.size())
                    {
                        return std::nullopt;
                    }
                    uint32_t code = 0;
                    for (int at = 0; at < 4; ++at)
                    {
                        const auto digit = m_text[m_position++];
                        code <<= 4;
                        if (digit >= '0' && digit <= '9') code |= static_cast<uint32_t>(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') code |= static_cast<uint32_t>(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') code |= static_cast<uint32_t>(digit - 'A' + 10);
                        else return std::nullopt;
                    }
                    // Surrogate pairs are outside what a claim name is for; keep the code point
                    // that fits and do not pretend to handle the rest.
                    if (code < 0x80)
                    {
                        text.push_back(static_cast<char>(code));
                    }
                    else if (code < 0x800)
                    {
                        text.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else
                    {
                        text.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Json> parseNumber()
    {
        const auto start = m_position;
        if (m_text[m_position] == '-')
        {
            ++m_position;
        }
        while (m_position < m_text.size() &&
               ((m_text[m_position] >= '0' && m_text[m_position] <= '9') ||
                m_text[m_position] == '.' || m_text[m_position] == 'e' || m_text[m_position] == 'E' ||
                m_text[m_position] == '+' || m_text[m_position] == '-'))
        {
            ++m_position;
        }
        Json value;
        value.type = Json::Type::Number;
        const std::string text(m_text.substr(start, m_position - start));
        char*             end = nullptr;
        value.number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size())
        {
            return std::nullopt;
        }
        return value;
    }

    std::optional<Json> parseArray(const size_t depth)
    {
        ++m_position; // '['
        Json value;
        value.type = Json::Type::Array;
        skipSpace();
        if (m_position < m_text.size() && m_text[m_position] == ']')
        {
            ++m_position;
            return value;
        }
        while (true)
        {
            auto item = parseValue(depth + 1);
            if (!item)
            {
                return std::nullopt;
            }
            value.items.push_back(std::move(*item));
            skipSpace();
            if (m_position >= m_text.size())
            {
                return std::nullopt;
            }
            if (m_text[m_position] == ',')
            {
                ++m_position;
                continue;
            }
            if (m_text[m_position] == ']')
            {
                ++m_position;
                return value;
            }
            return std::nullopt;
        }
    }

    std::optional<Json> parseObject(const size_t depth)
    {
        ++m_position; // '{'
        Json value;
        value.type = Json::Type::Object;
        skipSpace();
        if (m_position < m_text.size() && m_text[m_position] == '}')
        {
            ++m_position;
            return value;
        }
        while (true)
        {
            skipSpace();
            auto name = parseString();
            if (!name)
            {
                return std::nullopt;
            }
            skipSpace();
            if (m_position >= m_text.size() || m_text[m_position] != ':')
            {
                return std::nullopt;
            }
            ++m_position;
            auto member = parseValue(depth + 1);
            if (!member)
            {
                return std::nullopt;
            }
            value.fields.emplace(std::move(*name), std::move(*member));
            skipSpace();
            if (m_position >= m_text.size())
            {
                return std::nullopt;
            }
            if (m_text[m_position] == ',')
            {
                ++m_position;
                continue;
            }
            if (m_text[m_position] == '}')
            {
                ++m_position;
                return value;
            }
            return std::nullopt;
        }
    }

public:
    std::optional<Json> parseDocument()
    {
        return parse();
    }

private:
    std::string_view m_text;
    size_t           m_position {0};
};

std::optional<Json> parseJson(const std::string_view text)
{
    JsonParser parser(text);
    return parser.parseDocument();
}

// ---------------------------------------------------------------------------------------------
// A token is three base64url parts; the first two are signed together with a dot between them.
// ---------------------------------------------------------------------------------------------

struct Token
{
    std::string signingInput; ///< header.payload, exactly as received.
    std::string signature;    ///< The raw signature bytes.
    Json        header;
    Json        claims;
};

bool looksLikeToken(const std::string_view password)
{
    size_t dots = 0;
    for (const char symbol: password)
    {
        if (symbol == '.')
        {
            ++dots;
        }
        else if (base64Value(symbol) < 0)
        {
            return false;
        }
    }
    return dots == 2 && password.find("..") == std::string_view::npos;
}

std::optional<Token> splitToken(const std::string_view password)
{
    const auto first = password.find('.');
    const auto second = password.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        password.find('.', second + 1) != std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto headerText = base64UrlDecode(password.substr(0, first));
    const auto claimsText = base64UrlDecode(password.substr(first + 1, second - first - 1));
    const auto signature = base64UrlDecode(password.substr(second + 1));
    if (!headerText || !claimsText || !signature)
    {
        return std::nullopt;
    }
    const auto header = parseJson(*headerText);
    const auto claims = parseJson(*claimsText);
    if (!header || !claims || header->type != Json::Type::Object ||
        claims->type != Json::Type::Object)
    {
        return std::nullopt;
    }
    Token token;
    token.signingInput = std::string(password.substr(0, second));
    token.signature = *signature;
    token.header = *header;
    token.claims = *claims;
    return token;
}

// ---------------------------------------------------------------------------------------------
// Keys: a shared secret, a configured public key, or whatever the issuer's key set currently says.
// ---------------------------------------------------------------------------------------------

using PkeyPtr = std::shared_ptr<EVP_PKEY>;

PkeyPtr publicKeyFromPem(const std::string& pem)
{
    BIO* raw = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (raw == nullptr)
    {
        return {};
    }
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw, BIO_free);
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    return PkeyPtr(key, EVP_PKEY_free);
}

/// Import a provider-managed public key. OpenSSL 3 owns the representation: constructing RSA or
/// EC_KEY objects and assigning them to EVP_PKEY bypasses providers and uses deprecated APIs.
PkeyPtr publicKeyFromParameters(const char* algorithm, OSSL_PARAM* parameters)
{
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, algorithm, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_fromdata_init(context.get()) != 1)
    {
        return {};
    }
    EVP_PKEY* rawKey = nullptr;
    const auto imported = EVP_PKEY_fromdata(context.get(), &rawKey, EVP_PKEY_PUBLIC_KEY, parameters);
    PkeyPtr key(rawKey, EVP_PKEY_free);
    if (imported != 1 || !key)
    {
        return {};
    }
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> check(
        EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr), EVP_PKEY_CTX_free);
    if (!check || EVP_PKEY_public_check(check.get()) != 1)
    {
        return {};
    }
    return key;
}

/// An RSA public key from the JWK fields `n` and `e`.
PkeyPtr rsaKeyFromJwk(const std::string_view modulus, const std::string_view exponent)
{
    const auto n = base64UrlDecode(modulus);
    const auto e = base64UrlDecode(exponent);
    if (!n || !e || n->empty() || e->empty())
    {
        return {};
    }
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> modulusNumber(
        BN_bin2bn(reinterpret_cast<const unsigned char*>(n->data()), static_cast<int>(n->size()), nullptr),
        BN_free);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> exponentNumber(
        BN_bin2bn(reinterpret_cast<const unsigned char*>(e->data()), static_cast<int>(e->size()), nullptr),
        BN_free);
    const std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(
        OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    if (!modulusNumber || !exponentNumber || !builder ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulusNumber.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponentNumber.get()) != 1)
    {
        return {};
    }
    // JWK integers are big-endian, while OSSL_PARAM integers use native byte order. Let the
    // parameter builder convert the BIGNUMs rather than reinterpreting the network bytes.
    const std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> parameters(
        OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    if (!parameters)
    {
        return {};
    }
    return publicKeyFromParameters("RSA", parameters.get());
}

/// A P-256 public key from the JWK fields `x` and `y`; other curves are not ES256.
PkeyPtr ecKeyFromJwk(const std::string_view curve, const std::string_view x, const std::string_view y)
{
    if (curve != "P-256")
    {
        return {};
    }
    const auto xBytes = base64UrlDecode(x);
    const auto yBytes = base64UrlDecode(y);
    if (!xBytes || !yBytes || xBytes->size() != 32 || yBytes->size() != 32)
    {
        return {};
    }
    std::string point;
    point.reserve(65);
    point.push_back(0x04); // uncompressed
    point += *xBytes;
    point += *yBytes;
    char group[] = "prime256v1";
    OSSL_PARAM parameters[] {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()),
        OSSL_PARAM_construct_end()
    };
    return publicKeyFromParameters("EC", parameters);
}

struct Jwk
{
    std::string keyId;
    std::string algorithm; ///< `alg` from the key, when it says.
    PkeyPtr     key;
};

std::vector<Jwk> keysFromJwks(const Json& document)
{
    std::vector<Jwk> keys;
    const auto*      list = document.find("keys");
    if (list == nullptr || list->type != Json::Type::Array)
    {
        return keys;
    }
    for (const auto& entry: list->items)
    {
        if (entry.type != Json::Type::Object)
        {
            continue;
        }
        const auto keyType = entry.stringAt("kty");
        const auto use = entry.stringAt("use");
        if (!use.empty() && use != "sig")
        {
            continue;
        }
        Jwk key;
        key.keyId = entry.stringAt("kid");
        key.algorithm = entry.stringAt("alg");
        if (keyType == "RSA")
        {
            key.key = rsaKeyFromJwk(entry.stringAt("n"), entry.stringAt("e"));
        }
        else if (keyType == "EC")
        {
            key.key = ecKeyFromJwk(entry.stringAt("crv"), entry.stringAt("x"), entry.stringAt("y"));
        }
        if (key.key)
        {
            keys.push_back(std::move(key));
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------------------------
// Verification.
// ---------------------------------------------------------------------------------------------

bool verifyHs256(const std::string& secret, const std::string_view signingInput,
                 const std::string_view signature)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(signingInput.data()), signingInput.size(), digest,
             &length) == nullptr)
    {
        return false;
    }
    return signature.size() == length &&
           CRYPTO_memcmp(digest, signature.data(), length) == 0;
}

bool verifyRs256(EVP_PKEY* key, const std::string_view signingInput, const std::string_view signature)
{
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                          EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1)
    {
        return false;
    }
    return EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                            reinterpret_cast<const unsigned char*>(signingInput.data()),
                            signingInput.size()) == 1;
}

/// JWT carries ECDSA signatures as the raw r||s pair, not as the DER structure OpenSSL verifies.
bool verifyEs256(EVP_PKEY* key, const std::string_view signingInput, const std::string_view signature)
{
    if (signature.size() != 64)
    {
        return false;
    }
    ECDSA_SIG* ecdsa = ECDSA_SIG_new();
    if (ecdsa == nullptr)
    {
        return false;
    }
    BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()), 32, nullptr);
    BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data() + 32), 32, nullptr);
    if (r == nullptr || s == nullptr || ECDSA_SIG_set0(ecdsa, r, s) != 1)
    {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(ecdsa);
        return false;
    }
    unsigned char* der = nullptr;
    const auto     derLength = i2d_ECDSA_SIG(ecdsa, &der);
    ECDSA_SIG_free(ecdsa);
    if (derLength <= 0)
    {
        return false;
    }
    struct OpenSslFree
    {
        void operator()(unsigned char* memory) const { OPENSSL_free(memory); }
    };
    const std::unique_ptr<unsigned char, OpenSslFree> holder(der);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                          EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1)
    {
        return false;
    }
    return EVP_DigestVerify(context.get(), der, static_cast<size_t>(derLength),
                            reinterpret_cast<const unsigned char*>(signingInput.data()),
                            signingInput.size()) == 1;
}

/// How a token failed. Authentication and an unavailable key service are different answers: one
/// refuses this client, the other says the extension cannot judge anybody just now.
enum class Failure
{
    None,
    Refused,   ///< The token is wrong, expired, or not ours.
    Unavailable ///< The keys could not be obtained or the algebra failed.
};

struct Verdict
{
    Failure     failure {Failure::None};
    Json        claims;
    std::string algorithm;
};

} // namespace
