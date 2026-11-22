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

namespace xmq::exporter {

inline constexpr int defaultTimeoutMs = 5000;
inline constexpr int defaultRetries = 3;

/// What a body is when no template is set: one line per source, with the names people ask for
/// first. A message has no level of its own, so it says "message" - which is also what tells the
/// two sources apart at the receiver.
inline constexpr const char* defaultTemplate =
    R"({"topic":"{{topic}}","message":"{{message}}","level":"{{level}}"})";

} // namespace xmq::exporter
