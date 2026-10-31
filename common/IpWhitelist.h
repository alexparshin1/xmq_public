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
#include <vector>

namespace xmq {

/**
 * @brief An IPv4 allow list, kept pre-parsed for the accept path.
 *
 * The text is a comma-separated list of IPv4 addresses, each optionally followed by a prefix
 * length: `10.1.1.1/24,10.1.2.123/32,10.1.1.242`. An empty text means "no restriction" and
 * matches everything; a non-empty one admits an address only when one of its entries matches.
 * The comparison short-circuits on the first match, so the first matching entry - or an empty
 * list - is what allows a connection.
 */
class IpWhitelist
{
public:
    IpWhitelist() = default;

    /**
     * @brief Parses the text into a matchable form.
     * @param text  Comma-separated addresses, or empty for "allow anyone".
     * @throws std::invalid_argument when an entry is not a valid IPv4 address or prefix.
     */
    explicit IpWhitelist(const std::string& text);

    /// Re-parses, replacing whatever the list held.
    void assign(const std::string& text);

    /// True when the list holds no entry, which admits every address.
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    /**
     * @brief Whether the address is admitted.
     * @param addrNetworkByteOrder  An IPv4 address as `sockaddr_in::sin_addr::s_addr` holds it.
     * @return true for an empty list, or when an entry matches.
     */
    [[nodiscard]] bool allows(std::uint32_t addrNetworkByteOrder) const noexcept;

    /// The text this list was parsed from, trimmed.
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

    /// Whether the text is a valid, empty or otherwise, allow list.
    [[nodiscard]] static bool isValid(const std::string& text);

private:
    struct Entry
    {
        std::uint32_t network; ///< Host byte order, already masked to the prefix.
        std::uint32_t mask;    ///< Host byte order.
    };

    std::vector<Entry> m_entries;
    std::string        m_text;
};

} // namespace xmq
