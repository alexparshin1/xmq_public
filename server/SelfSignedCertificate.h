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

#include <filesystem>
#include <sptk5/cutils>

namespace xmq {

/**
 * @brief A certificate the server issues to itself.
 *
 * The configuration interface is served over TLS, and a server that needed a certificate prepared
 * before it would answer at all would leave a new installation with nothing to configure it from.
 * So one is generated on first use.
 *
 * It is self-signed, which browsers do not trust: the first visit shows a warning, and it is worth
 * accepting only after checking the fingerprint the server logged. Replacing the two files with a
 * certificate from a CA the browsers already know removes the warning, and nothing else has to
 * change - the server serves whatever it finds at those paths.
 */
class XMQ_EXPORT SelfSignedCertificate
{
public:
    /**
     * @brief Create a certificate and key at these paths, unless they are already there.
     *
     * Both files have to be missing for a pair to be issued. One without the other is left alone
     * and reported as an error: overwriting half of somebody's certificate is not a repair.
     *
     * @param certificateFile   Where the certificate goes.
     * @param privateKeyFile    Where the private key goes; created readable only by its owner.
     * @param hostName          Name the certificate is made out to, and its first subject
     *                          alternative name. The name others reach this server by, which is
     *                          not always the name it calls itself - see xmq::thisHostName().
     * @param description       Receives what was created, for the log.
     * @return true if a pair was issued, false if a usable one was already in place.
     * @throws sptk::Exception when the pair could not be created.
     */
    static bool create(const std::filesystem::path& certificateFile,
                       const std::filesystem::path& privateKeyFile,
                       const sptk::String&          hostName,
                       sptk::String&                description);

    /**
     * @brief Issue a certificate and key, replacing whatever is at those paths.
     *
     * What create() does for an installation that has none, for the moment an installation is
     * being set up from scratch: the previous files are kept beside the new ones with an '.old'
     * suffix rather than removed.
     *
     * @param certificateFile   Where the certificate goes.
     * @param privateKeyFile    Where the private key goes; created readable only by its owner.
     * @param hostName          Name the certificate is made out to.
     * @param description       Receives what was created, for the log.
     * @throws sptk::Exception when the pair could not be created.
     */
    static void reissue(const std::filesystem::path& certificateFile,
                        const std::filesystem::path& privateKeyFile,
                        const sptk::String&          hostName,
                        sptk::String&                description);

    /**
     * @brief What a certificate says about itself: subject, expiry, and SHA-256 fingerprint.
     *
     * Logged at startup so that the fingerprint a browser shows can be compared against something
     * other than the browser - which is the only thing that makes accepting a self-signed
     * certificate meaningfully different from accepting any certificate at all.
     *
     * @param certificateFile   Certificate to describe.
     * @return the description, or empty when the file cannot be read.
     */
    [[nodiscard]] static sptk::String describe(const std::filesystem::path& certificateFile);

private:
    /**
     * @brief Write a new pair to these paths, whatever is there.
     * @param certificateFile   Where the certificate goes.
     * @param privateKeyFile    Where the private key goes.
     * @param hostName          Name the certificate is made out to.
     * @param description       Receives what was created.
     * @throws sptk::Exception when the pair could not be created.
     */
    static void issue(const std::filesystem::path& certificateFile,
                      const std::filesystem::path& privateKeyFile,
                      const sptk::String&          hostName,
                      sptk::String&                description);
};

} // namespace xmq
