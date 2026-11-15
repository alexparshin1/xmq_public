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
 * @brief JWKS key import helpers used by the JWT extension.
 */

#include "extension/XmqExtension.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/param_build.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sptk5/JWT.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PkeyPtr = std::shared_ptr<EVP_PKEY>;

struct Jwk
{
    std::string keyId;
    std::string algorithm;
    PkeyPtr key;
};

/// XMQ's JWK/OpenSSL adapter. SPTK owns JSON/base64url and JWT verification; key import stays here.
class XmqJwkCodec final
{
public:
    static std::string publicKeyPem(EVP_PKEY* key)
    {
        if (!key) throw std::runtime_error("JWT public key is missing");
        const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        if (!bio || PEM_write_bio_PUBKEY(bio.get(), key) != 1)
            throw std::runtime_error("JWT key serialization failed");
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio.get(), &data);
        if (length <= 0 || !data)
            throw std::runtime_error("JWT key serialization failed");
        return {data, static_cast<size_t>(length)};
    }
// ---------------------------------------------------------------------------------------------
// base64url, which is base64 with two characters changed and no padding.
// ---------------------------------------------------------------------------------------------

static std::optional<std::string> base64UrlDecode(const std::string_view body)
{
    // Decoded here rather than through the SPTK helper: that function is a free function without
    // the DLL export marker, so a Windows build of this extension cannot link against it. The JWK
    // fields are the only place this extension needs base64url, and RFC 4648's alphabet is short
    // enough to spell out.
    //
    // The URL-safe alphabet only, and no padding: a padded value is what a re-encoder produces,
    // not what a signer could have emitted, and accepting it would let two spellings of one key
    // reach different code paths.
    std::string decoded;
    decoded.reserve(body.size() * 3 / 4);
    unsigned int accumulator = 0;
    int          bits = 0;
    for (const char ch: body)
    {
        unsigned int value = 0;
        if (ch >= 'A' && ch <= 'Z')
        {
            value = static_cast<unsigned int>(ch - 'A');
        }
        else if (ch >= 'a' && ch <= 'z')
        {
            value = static_cast<unsigned int>(ch - 'a') + 26U;
        }
        else if (ch >= '0' && ch <= '9')
        {
            value = static_cast<unsigned int>(ch - '0') + 52U;
        }
        else if (ch == '-')
        {
            value = 62U;
        }
        else if (ch == '_')
        {
            value = 63U;
        }
        else
        {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    // A group of six leftover bits cannot come from any whole byte, and leftover bits that are
    // not zero mean the value was cut short rather than padded.
    if (bits >= 6 || (accumulator & ((1U << bits) - 1U)) != 0)
    {
        return std::nullopt;
    }
    return decoded;
}

// ---------------------------------------------------------------------------------------------
// Keys: a shared secret, a configured public key, or whatever the issuer's key set currently says.
// ---------------------------------------------------------------------------------------------

static PkeyPtr publicKeyFromPem(const std::string& pem)
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
static PkeyPtr publicKeyFromParameters(const char* algorithm, OSSL_PARAM* parameters)
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
static PkeyPtr rsaKeyFromJwk(const std::string_view modulus, const std::string_view exponent)
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
static PkeyPtr ecKeyFromJwk(const std::string_view curve, const std::string_view x, const std::string_view y)
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

static std::string jwkString(const sptk::xdoc::SNode& node, const char* name)
{
    if (!node || node->type() != sptk::xdoc::Node::Type::Object)
        return {};
    const auto child = node->findFirst(name, sptk::xdoc::SearchMode::ImmediateChild);
    const auto value = child ? child->textValue() : std::nullopt;
    return value ? std::string(*value) : std::string{};
}

static std::vector<Jwk> keysFromJwks(const sptk::xdoc::SNode& document)
{
    std::vector<Jwk> keys;
    if (!document || document->type() != sptk::xdoc::Node::Type::Object)
        return keys;
    const auto list = document->findFirst("keys", sptk::xdoc::SearchMode::ImmediateChild);
    if (!list || list->type() != sptk::xdoc::Node::Type::Array)
    {
        return keys;
    }
    for (const auto& entry: list->nodes())
    {
        if (entry->type() != sptk::xdoc::Node::Type::Object)
        {
            continue;
        }
        const auto keyType = jwkString(entry, "kty");
        const auto use = jwkString(entry, "use");
        if (!use.empty() && use != "sig")
        {
            continue;
        }
        Jwk key;
        key.keyId = jwkString(entry, "kid");
        key.algorithm = jwkString(entry, "alg");
        if (keyType == "RSA")
        {
            key.key = rsaKeyFromJwk(jwkString(entry, "n"), jwkString(entry, "e"));
        }
        else if (keyType == "EC")
        {
            key.key = ecKeyFromJwk(jwkString(entry, "crv"), jwkString(entry, "x"),
                                   jwkString(entry, "y"));
        }
        if (key.key)
        {
            keys.push_back(std::move(key));
        }
    }
    return keys;
}
};

} // namespace
