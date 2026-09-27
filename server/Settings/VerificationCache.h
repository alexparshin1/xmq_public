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


#pragma once

#include "base/xmq.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

namespace xmq {

/**
 * @brief Remembers what verifying a password answered, so the cost is paid once.
 *
 * A costed KDF is affordable only because of this. Measured on the bench: PBKDF2-HMAC-SHA256 at
 * 10000 iterations is 1.13 ms, or 884 verifications per second per core, and the CONNECT path runs
 * at thousands per second. What a cache of accounts cannot do is help - the record is in memory
 * either way and the cost is the arithmetic, not the lookup - so what is kept here is the
 * *decision*.
 *
 * Refusals are kept as well as admissions. Otherwise a client sending wrong passwords costs a full
 * KDF each time, which is a way to spend the broker's cores from outside. A corrected password
 * hashes to a different entry and is verified properly, so keeping refusals delays no repair.
 *
 * Nothing here holds a password. An entry is found by an HMAC of the credentials under a key made
 * when the process starts and never written anywhere; the key dies with the process, so a core file
 * or a heap dump yields nothing that can be replayed against another broker.
 *
 * Direct-mapped and fixed in size: a collision overwrites, and a miss is not a failure - it falls
 * through to a real verification, which is correct and merely slower. Sizing is therefore a tuning
 * choice and never a correctness one, exactly as for the decision cache in AclGroup.
 */
class VerificationCache
{
public:
    /// What the cache knows about one set of credentials.
    enum class Answer
    {
        Unknown, ///< Not remembered, or remembered too long ago: verify it properly.
        Allowed,
        Refused
    };

    VerificationCache();

    /**
     * @brief Size the cache for the accounts it has to serve.
     *
     * Called whenever the accounts are loaded or reloaded. The size follows the number of accounts
     * rather than a number chosen in advance, because the two deployments this has to suit are an
     * office with five accounts and a field with a million devices holding one each, and no single
     * figure is right for both. Rounded up to a power of two, floored so that a small installation
     * still absorbs its reconnections, and capped so that a store reporting something absurd cannot
     * ask for all of memory.
     *
     * Entries do not survive it: a resize is a change of accounts, which is exactly when what was
     * remembered stops being trustworthy.
     */
    void sizeFor(size_t accounts);

    /// What was answered for these credentials, if anything still is.
    [[nodiscard]] Answer look(const std::string& username, const std::string& password) const;

    /// Keep what verifying them answered.
    void remember(const std::string& username, const std::string& password, bool allowed);

    /**
     * @brief Drop everything remembered.
     *
     * Called when an account changes. A generation counter rather than a sweep of the table: the
     * cost is one store, whatever the table's size, and every entry made before it stops matching
     * at once. The same mechanism AclGroup uses, deliberately - a password change and a rights
     * change must not reach the broker's caches by two different routes.
     */
    void forget();

    /// How many entries the table holds. For tests and for the log line that reports the sizing.
    [[nodiscard]] size_t capacity() const;

private:
    /// A cache entry, holding no password and no username.
    struct Slot
    {
        std::array<uint8_t, 32> m_key {};        ///< HMAC of the credentials; all zero when unused.
        uint64_t                m_generation {}; ///< Zero for an untouched slot, which matches nothing.
        int64_t                 m_expiresAtMs {};
        bool                    m_allowed {};
    };

    /// The HMAC of these credentials under this process's key.
    [[nodiscard]] std::array<uint8_t, 32> keyFor(const std::string& username,
                                                 const std::string& password) const;

    static constexpr size_t smallestTable = 1024;

    /// 64 MB of slots. A store answering more than this is answering wrongly.
    static constexpr size_t largestTable = 1U << 20U;

    /// Long enough that a burst of reconnections is served from here, short enough that an account
    /// disabled in a store this broker is not told about - a directory, once authentication is an
    /// extension - stops connecting while somebody is still watching.
    static constexpr int64_t lifetimeMs = 60000;

    /// Made at start-up from the system's random source, never stored, never logged.
    std::array<uint8_t, 32> m_processKey {};

    mutable std::shared_mutex m_lock;
    std::vector<Slot>         m_slots;
    uint64_t                  m_generation {1};
};

} // namespace xmq
