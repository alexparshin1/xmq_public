/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2025 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/


#pragma once

constexpr auto* REACT_DIRECTORY = "@REACT_DIRECTORY@";

// Where the broker keeps its configuration, its certificates and its log. Decided by CMake, which
// knows the installation prefix and the platform, rather than by #ifdef here: an installation under
// the user's own home has to answer differently from a packaged one, and that is a property of how
// it was built, not of what it was built on.
constexpr auto* XMQ_CONF_DIRECTORY = "@XMQ_CONF_DIRECTORY@";
constexpr auto* XMQ_CERTS_DIRECTORY = "@XMQ_CERTS_DIRECTORY@";
constexpr auto* XMQ_LOGS_DIRECTORY = "@XMQ_LOGS_DIRECTORY@";

constexpr auto* XMQ_VERSION_NUMBER = "@VERSION@";
