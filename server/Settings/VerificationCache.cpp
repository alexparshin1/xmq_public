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


#include "VerificationCache.h"

#include <sptk5/Exception.h>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <bit>
#include <chrono>
#include <cstring>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

int64_t nowMs()
{
    // Steady, not wall: an entry's lifetime is a duration and must not be lengthened or cut short
    // by the clock being corrected. A host here was sixteen hours out.
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

VerificationCache::VerificationCache()
{
    if (RAND_bytes(m_processKey.data(), static_cast<int>(m_processKey.size())) != 1)
    {
        throw Exception("Cannot get random bytes for the verification cache key");
    }

    m_slots.assign(smallestTable, Slot {});
}

void VerificationCache::sizeFor(const size_t accounts)
{
    // Twice the accounts, so that a table addressed by hash is not run at full occupancy, where
    // collisions - and with them evictions of entries still wanted - become the common case rather
    // than the exception.
    size_t wanted = smallestTable;
    if (accounts > smallestTable / 2)
    {
        wanted = bit_ceil(accounts * 2);
    }
    wanted = min(wanted, largestTable);

    const unique_lock lock(m_lock);
    if (wanted == m_slots.size())
    {
        // Still dropped: this is called when the accounts are loaded, and an account that has just
        // been read may be the one that changed.
        ++m_generation;
        return;
    }

    m_slots.assign(wanted, Slot {});
    ++m_generation;
}

array<uint8_t, 32> VerificationCache::keyFor(const string& username, const string& password) const
{
    // The separator matters: without it ("ab", "c") and ("a", "bc") are one set of credentials, and
    // a password would admit the wrong account.
    string material;
    material.reserve(username.size() + password.size() + 1);
    material.append(username);
    material.push_back('\0');
    material.append(password);

    array<uint8_t, 32> key {};
    unsigned int       length = 0;
    HMAC(EVP_sha256(), m_processKey.data(), static_cast<int>(m_processKey.size()),
         reinterpret_cast<const unsigned char*>(material.data()), material.size(), key.data(), &length);

    OPENSSL_cleanse(material.data(), material.size());
    return key;
}

VerificationCache::Answer VerificationCache::look(const string& username, const string& password) const
{
    // Outside the lock: it is the only arithmetic here, and it needs nothing the lock protects.
    const auto key = keyFor(username, password);

    const shared_lock lock(m_lock);

    const auto& slot = m_slots[(static_cast<size_t>(key[0]) | static_cast<size_t>(key[1]) << 8U |
                                static_cast<size_t>(key[2]) << 16U | static_cast<size_t>(key[3]) << 24U) %
                               m_slots.size()];

    if (slot.m_generation != m_generation || nowMs() >= slot.m_expiresAtMs)
    {
        return Answer::Unknown;
    }

    // Constant-time, like the verification it stands in for: this comparison decides whether a
    // client is admitted, and the number of leading bytes that matched is not something a caller
    // should be able to measure.
    if (CRYPTO_memcmp(slot.m_key.data(), key.data(), key.size()) != 0)
    {
        return Answer::Unknown;
    }

    return slot.m_allowed ? Answer::Allowed : Answer::Refused;
}

void VerificationCache::remember(const string& username, const string& password, const bool allowed)
{
    const auto key = keyFor(username, password);

    const unique_lock lock(m_lock);

    auto& slot = m_slots[(static_cast<size_t>(key[0]) | static_cast<size_t>(key[1]) << 8U |
                          static_cast<size_t>(key[2]) << 16U | static_cast<size_t>(key[3]) << 24U) %
                         m_slots.size()];

    slot.m_key = key;
    slot.m_generation = m_generation;
    slot.m_expiresAtMs = nowMs() + lifetimeMs;
    slot.m_allowed = allowed;
}

void VerificationCache::forget()
{
    const unique_lock lock(m_lock);
    ++m_generation;
}

size_t VerificationCache::capacity() const
{
    const shared_lock lock(m_lock);
    return m_slots.size();
}
