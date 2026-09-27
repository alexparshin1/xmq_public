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

#include <sptk5/cutils>

#include <string>

namespace xmq {

/**
 * @brief One-way password verifiers, and the checking of them.
 *
 * What is stored is `algorithm$iterations$salt$hash`, so that the algorithm and the cost are part
 * of the record rather than of the code that reads it. A password whose stored form names an older
 * cost can be re-hashed at its next successful login and nobody has to be asked to choose a new
 * one - which is why the cost may be raised later without a migration.
 *
 * PBKDF2-HMAC-SHA256 rather than a bare digest: SHA-256 is fast by design, which is the wrong
 * property here, and without a salt equal passwords produce equal hashes and fall to precomputed
 * tables. Rather than Argon2, which would be the better answer, because it arrived in OpenSSL 3.2
 * and Ubuntu 24.04 - supported until 2029 - ships 3.0.13: using it means either carrying libargon2
 * or dropping a platform we support.
 */
class XMQ_EXPORT PasswordHash
{
public:
    /**
     * @brief Iterations for a newly hashed password when the configuration names none.
     *
     * A thousandth of a second per verification here, against roughly a ten-millionth for a bare
     * digest: the same work that would try ten million passwords a second on a stolen table tries
     * a thousand. The salt is what makes one table useless against every account at once; this is
     * the margin on top of it, and how much margin to buy is the operator's choice - hence
     * password_iterations in the configuration.
     *
     * The default stays where a broker admitting five thousand clients a second can afford it on a
     * few cores, because this runs on the CONNECT path and not on a login form.
     */
    static constexpr int defaultIterations = 10000;

    /// Below this PBKDF2 has nothing to do; a configured 0 or a negative number means this.
    static constexpr int minimumIterations = 1;

    /**
     * @brief Hashes a password, with a salt of its own.
     * @param password      The clear-text password.
     * @param iterations    Cost, in PBKDF2 iterations.
     * @return The stored form: pbkdf2-sha256$iterations$salt$hash, salt and hash in hex.
     */
    static sptk::String hash(const std::string& password, int iterations = defaultIterations);

    /**
     * @brief Whether a password matches a stored verifier.
     *
     * The comparison is constant-time: an early return on the first differing byte tells anyone who
     * can time it how much of a guess was right, which turns guessing a hash into guessing it one
     * byte at a time.
     *
     * This costs the full iteration count every time it is called, and it is called on the CONNECT
     * path, so a broker admitting many clients at once needs the answer cached rather than this
     * made cheap - lowering the cost is the one response that helps an attacker more than it helps
     * the broker.
     *
     * @param password      The clear-text password offered.
     * @param stored        The stored form, as hash() produced it.
     * @return True when they match. False for anything unparseable, never an exception.
     */
    static bool verify(const std::string& password, const std::string& stored);

    /**
     * @brief Whether a stored verifier was made with settings this build no longer uses.
     * @remarks True means it should be replaced on the next successful login. A verifier that
     *          cannot be parsed is not "outdated" - it is broken, and re-hashing would hide that.
     */
    static bool needsRehash(const std::string& stored, int iterations = defaultIterations);

    /// Whether this looks like a stored verifier at all, as opposed to a password in clear text.
    static bool isHashed(const std::string& stored);

    /// Shortest password accepted.
    static constexpr size_t minimumLength = 8;

    /**
     * @brief What is wrong with a password somebody has just chosen, if anything.
     *
     * Asked where a person picks a password and nowhere else. A password that arrives from
     * somewhere else - migrated from the accounts file of an older broker, or generated for the
     * cluster account - is not judged: refusing it would break an installation that worked
     * yesterday, and nobody would be able to fix it, since fixing it means signing in.
     *
     * @param password  The clear-text password.
     * @return Empty when it is acceptable, or one sentence naming everything it lacks - all of it
     *         at once, because being told one missing thing at a time is how a person makes five
     *         attempts at what could have been one.
     */
    [[nodiscard]] static sptk::String weakness(const std::string& password);

    /// Throws when weakness() has something to say. For the paths that only need to refuse.
    static void refuseIfWeak(const std::string& password);
};

} // namespace xmq
