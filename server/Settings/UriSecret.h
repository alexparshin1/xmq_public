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

namespace xmq {

/**
 * @brief Hiding the password inside a connection URI, and putting it back.
 *
 * A URI like postgresql://someone:secret@dbhost/accounts carries a password, and the interface asks
 * for these settings and shows them. Bridge passwords have always been sent out as '*****' and
 * matched back on the way in; connection URIs were sent out whole, which is the same secret with
 * none of the care.
 */
class XMQ_EXPORT UriSecret
{
public:
    /// What a hidden password looks like, and what comes back when it was not changed.
    static constexpr std::string_view mask = "*****";

    /**
     * @brief The same URI with its password replaced by the mask.
     * @remarks A URI with no credentials, or with a username and no password, is returned as it is
     *          - there is nothing to hide and inventing a mask would invent a password.
     */
    [[nodiscard]] static sptk::String hidden(const sptk::String& uri);

    /**
     * @brief The URI to store, given what came back from the interface.
     * @param given     What was sent back, possibly still carrying the mask.
     * @param stored    What is on record now.
     * @return `given` with the mask replaced by the stored password, or `given` unchanged when it
     *         carries a real password - which is how a password is changed.
     */
    [[nodiscard]] static sptk::String restored(const sptk::String& given, const sptk::String& stored);

private:
    /// Where the password sits in a URI, or npos twice when it has none.
    static void findPassword(const sptk::String& uri, size_t& start, size_t& length);
};

} // namespace xmq
