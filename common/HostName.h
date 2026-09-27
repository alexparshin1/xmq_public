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

#include <sptk5/cutils>

namespace xmq {

/**
 * @brief The name this machine calls itself.
 *
 * A starting point rather than an answer: it is the right host name for a server nobody reaches
 * from anywhere else, and it is only a guess for one that other nodes connect to, which may know
 * it by an alias, an address, or a name that only resolves outside this machine. Wherever it is
 * offered it should be possible to say otherwise.
 *
 * @return this machine's host name, or "localhost" when it cannot be read.
 */
[[nodiscard]] sptk::String thisHostName();

} // namespace xmq
