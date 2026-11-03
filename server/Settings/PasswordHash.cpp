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


#include "PasswordHash.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <sptk5/Base64.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <charconv>
#include <vector>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

constexpr string_view algorithmName = "pbkdf2-sha256";
constexpr size_t      saltBytes = 16;
constexpr size_t      hashBytes = 32;

String toHex(const unsigned char* bytes, const size_t count)
{
    static constexpr string_view digits = "0123456789abcdef";

    String hex;
    hex.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        hex += digits[bytes[i] >> 4U];
        hex += digits[bytes[i] & 0x0FU];
    }
    return hex;
}

/// Returns false rather than throwing on anything that is not an even run of hex digits.
bool fromHex(const string_view hex, vector<unsigned char>& bytes)
{
    if (hex.empty() || hex.size() % 2 != 0)
    {
        return false;
    }

    bytes.resize(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        unsigned value = 0;
        const auto [end, error] = from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, value, 16);
        if (error != errc {} || end != hex.data() + i * 2 + 2)
        {
            return false;
        }
        bytes[i] = static_cast<unsigned char>(value);
    }
    return true;
}

/// The four fields of the stored form, or false if it is not in that form at all.
bool split(const string& stored, string_view& algorithm, int& iterations,
           string_view& salt, string_view& hash)
{
    const string_view view(stored);

    const auto first = view.find('$');
    if (first == string_view::npos)
    {
        return false;
    }
    const auto second = view.find('$', first + 1);
    if (second == string_view::npos)
    {
        return false;
    }
    const auto third = view.find('$', second + 1);
    if (third == string_view::npos)
    {
        return false;
    }

    algorithm = view.substr(0, first);
    salt = view.substr(second + 1, third - second - 1);
    hash = view.substr(third + 1);

    const auto iterationsText = view.substr(first + 1, second - first - 1);
    const auto [end, error] = from_chars(iterationsText.data(),
                                         iterationsText.data() + iterationsText.size(), iterations);
    if (error != errc {} || end != iterationsText.data() + iterationsText.size() || iterations <= 0)
    {
        return false;
    }

    return !algorithm.empty() && !salt.empty() && !hash.empty();
}

/**
 * @brief Whether this is a Mosquitto 2.0 verifier: `$7$iterations$base64-salt$base64-hash`.
 *
 * Mosquitto's password file is the one other format XMQ reads. It is PBKDF2-HMAC-SHA512 with the
 * cost in the string, so it can be checked as it stands - there is no way to re-hash a password
 * nobody knows. `needsRehash()` says true for it, which lets a login replace it with this class's
 * stronger verifier once the clear password is in hand.
 */
bool isMosquittoPbkdf2(const string_view stored)
{
    return stored.starts_with("$7$");
}

bool verifyMosquitto(const string& password, const string_view stored)
{
    // $7$iterations$salt$hash
    const auto third = stored.find('$', 3);
    if (third == string_view::npos)
    {
        return false;
    }
    const auto fourth = stored.find('$', third + 1);
    if (fourth == string_view::npos)
    {
        return false;
    }

    int iterations = 0;
    const auto iterationText = stored.substr(3, third - 3);
    const auto [end, error] = from_chars(iterationText.data(),
                                         iterationText.data() + iterationText.size(), iterations);
    if (error != errc {} || end != iterationText.data() + iterationText.size() || iterations <= 0)
    {
        return false;
    }

    Buffer salt;
    Buffer expected;
    try
    {
        Base64::decode(salt, String(string(stored.substr(third + 1, fourth - third - 1))));
        Base64::decode(expected, String(string(stored.substr(fourth + 1))));
    }
    catch (const Exception&)
    {
        return false;
    }
    if (salt.bytes() == 0 || expected.bytes() != 64) // SHA-512 is 64 bytes
    {
        return false;
    }

    vector<unsigned char> derived(64);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.bytes()),
                          iterations, EVP_sha512(),
                          static_cast<int>(derived.size()), derived.data()) != 1)
    {
        return false;
    }
    return CRYPTO_memcmp(derived.data(), expected.data(), derived.size()) == 0;
}

/// PBKDF2-HMAC-SHA256 of one password against one salt.
bool derive(const string& password, const vector<unsigned char>& salt, const int iterations,
            const size_t length, vector<unsigned char>& derived)
{
    derived.resize(length);
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             salt.data(), static_cast<int>(salt.size()),
                             iterations, EVP_sha256(),
                             static_cast<int>(derived.size()), derived.data()) == 1;
}

} // namespace

String PasswordHash::hash(const string& password, const int iterations)
{
    // A configuration is a place where 0 gets written, by hand or by a form that left the field
    // empty, and PBKDF2 has nothing to do with fewer than one round. Refusing the whole hash over
    // it would lock an administrator out of a broker for a typo in an unrelated field.
    const auto rounds = std::max(iterations, minimumIterations);

    array<unsigned char, saltBytes> salt {};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
    {
        // Without a salt from the system's own generator there is nothing safe to store, and
        // inventing one here - from a clock, or a counter - would be worse than saying so.
        throw Exception("Cannot generate a password salt: the system random source failed");
    }

    const vector saltBuffer(salt.begin(), salt.end());
    vector<unsigned char> derived;
    if (!derive(password, saltBuffer, rounds, hashBytes, derived))
    {
        throw Exception("Cannot hash the password: PBKDF2 failed");
    }

    return String(string(algorithmName)) + "$" + to_string(rounds) + "$" +
           toHex(salt.data(), salt.size()) + "$" + toHex(derived.data(), derived.size());
}

bool PasswordHash::verify(const string& password, const string& stored)
{
    if (isMosquittoPbkdf2(stored))
    {
        return verifyMosquitto(password, stored);
    }

    string_view algorithm;
    string_view saltHex;
    string_view hashHex;
    int         iterations = 0;

    if (!split(stored, algorithm, iterations, saltHex, hashHex) || algorithm != algorithmName)
    {
        return false;
    }

    vector<unsigned char> salt;
    vector<unsigned char> expected;
    if (!fromHex(saltHex, salt) || !fromHex(hashHex, expected))
    {
        return false;
    }

    vector<unsigned char> derived;
    if (!derive(password, salt, iterations, expected.size(), derived))
    {
        return false;
    }

    return CRYPTO_memcmp(derived.data(), expected.data(), expected.size()) == 0;
}

bool PasswordHash::needsRehash(const string& stored, const int iterations)
{
    // A Mosquitto verifier is checkable but weaker than this class's default, so a successful
    // login is the moment to replace it - the password is the one thing the import never had.
    if (isMosquittoPbkdf2(stored))
    {
        return true;
    }

    string_view algorithm;
    string_view salt;
    string_view hash;
    int         storedIterations = 0;

    if (!split(stored, algorithm, storedIterations, salt, hash))
    {
        return false;
    }

    // A lower configured cost is not a reason to weaken a stored password.
    return algorithm != algorithmName || storedIterations < iterations;
}

bool PasswordHash::isHashed(const string& stored)
{
    if (isMosquittoPbkdf2(stored))
    {
        // Only if it is a complete, decodable verifier; a truncated one is broken, not a hash.
        const auto third = stored.find('$', 3);
        const auto fourth = third == string::npos ? string::npos : stored.find('$', third + 1);
        if (third == string::npos || fourth == string::npos)
        {
            return false;
        }
        try
        {
            Buffer salt;
            Buffer hash;
            Base64::decode(salt, String(stored.substr(third + 1, fourth - third - 1)));
            Base64::decode(hash, String(stored.substr(fourth + 1)));
            return salt.bytes() > 0 && hash.bytes() == 64;
        }
        catch (const Exception&)
        {
            return false;
        }
    }

    string_view algorithm;
    string_view saltHex;
    string_view hashHex;
    int         iterations = 0;

    if (!split(stored, algorithm, iterations, saltHex, hashHex) || algorithm != algorithmName)
    {
        return false;
    }

    // The fields have to be readable, not merely present. A verifier this cannot decode is broken,
    // and calling it a hash would let the migration walk past a row it needs to replace.
    vector<unsigned char> salt;
    vector<unsigned char> expected;
    return fromHex(saltHex, salt) && fromHex(hashHex, expected);
}

String PasswordHash::weakness(const string& password)
{
    Strings missing;

    if (password.length() < minimumLength)
    {
        missing.push_back("at least " + to_string(minimumLength) + " characters");
    }


    const auto has = [&password](const auto& predicate)
    {
        return ranges::any_of(password, [&predicate](const unsigned char character)
                              { return predicate(character); });
    };

    if (!has([](const unsigned char c) { return std::islower(c) != 0; }))
    {
        missing.push_back("at least one lowercase letter");
    }
    if (!has([](const unsigned char c) { return std::isupper(c) != 0; }))
    {
        missing.push_back("at least one uppercase letter");
    }
    if (!has([](const unsigned char c) { return std::isdigit(c) != 0; }))
    {
        missing.push_back("at least one digit");
    }
    if (!has([](const unsigned char c) { return std::ispunct(c) != 0; }))
    {
        missing.push_back("at least one punctuation character");
    }

    if (missing.empty())
    {
        return {};
    }

    // "A and B", or "A, B and C" - a bare comma-separated list reads as an unfinished sentence,
    // and this one is read by somebody who has just been refused and wants to know what to type.
    String requirements(missing.back());
    if (missing.size() > 1)
    {
        const Strings allButLast(missing.begin(), missing.end() - 1);
        requirements = allButLast.join(", ") + " and " + requirements;
    }

    return "The password needs " + requirements + ".";
}

void PasswordHash::refuseIfWeak(const string& password)
{
    if (const auto reason = weakness(password);
        !reason.empty())
    {
        throw Exception(reason);
    }
}
