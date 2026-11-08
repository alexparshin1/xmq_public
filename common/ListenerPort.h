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

#include <cstdint>
#include <string>

namespace xmq {

/**
 * @brief Whether a listener could open the address and port.
 *
 * Asked without SO_REUSEPORT, because that is what the broker's listeners ask the kernel for and
 * it is what makes a second broker able to share a port with a first one instead of failing. The
 * probe is closed before returning, so a true answer leaves the port as it was found.
 *
 * @param bindAddress   IPv4 address to bind, or empty for every address.
 * @param port          TCP port to test. Zero is always available, as it asks for any port.
 * @return true when nothing else on this host holds the address and port.
 */
[[nodiscard]] bool isListenerPortAvailable(const std::string& bindAddress, std::uint16_t port);

/**
 * @brief Throws unless a listener could open the address and port.
 *
 * The check belongs before addListener(): once the broker's own listening socket is open, its
 * SO_REUSEPORT makes the same check pass and the conflict is invisible.
 *
 * @param listener      What the error should name, such as "Listener Default (MQTT)".
 * @param bindAddress   IPv4 address the listener would use, or empty for every address.
 * @param port          TCP port the listener would use.
 * @throws sptk::Exception naming the listener, address and port when the port is already in use.
 */
void checkListenerPortAvailable(const std::string& listener, const std::string& bindAddress,
                                std::uint16_t port);

} // namespace xmq
