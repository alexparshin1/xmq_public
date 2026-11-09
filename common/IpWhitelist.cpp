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

#include "IpWhitelist.h"

#include <cctype>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace xmq {

namespace {

/// A network-byte-order IPv4 address, as sin_addr holds it, in host byte order.
std::uint32_t networkToHost(std::uint32_t value)
{
    return ntohl(value);
}

/// One `a.b.c.d` address as a host-byte-order integer, throwing when malformed.
std::uint32_t parseAddress(const std::string& token)
{
    std::uint32_t octets[4]{};
    int           count = 0;
    for (const auto c: token)
    {
        if (c == '.')
        {
            if (++count >= 4)
            {
                throw std::invalid_argument("Too many octets in '" + token + "'.");
            }
            continue;
        }
        if (c < '0' || c > '9')
        {
            throw std::invalid_argument("'" + token + "' is not an IPv4 address.");
        }
        octets[count] = octets[count] * 10 + static_cast<std::uint32_t>(c - '0');
        if (octets[count] > 255)
        {
            throw std::invalid_argument("Octet out of range in '" + token + "'.");
        }
    }
    if (count != 3)
    {
        throw std::invalid_argument("'" + token + "' is not an IPv4 address.");
    }
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

std::uint32_t parsePrefix(const std::string& token)
{
    if (token.empty())
    {
        throw std::invalid_argument("A prefix length is missing after '/'.");
    }
    std::uint32_t prefix = 0;
    for (const auto c: token)
    {
        if (c < '0' || c > '9')
        {
            throw std::invalid_argument("'" + token + "' is not a prefix length.");
        }
        prefix = prefix * 10 + static_cast<std::uint32_t>(c - '0');
        if (prefix > 32)
        {
            throw std::invalid_argument("Prefix length out of range in '" + token + "'.");
        }
    }
    return prefix;
}

std::string trim(const std::string& text)
{
    auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return {};
    }
    auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

IpWhitelist::IpWhitelist(const std::string& text)
{
    assign(text);
}

void IpWhitelist::assign(const std::string& text)
{
    m_entries.clear();
    m_text = trim(text);

    if (m_text.empty())
    {
        return;
    }

    size_t begin = 0;
    while (begin <= m_text.size())
    {
        const auto comma = m_text.find(',', begin);
        const auto entryText = trim(m_text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (entryText.empty())
        {
            throw std::invalid_argument("An IP white list entry is empty.");
        }

        const auto slash = entryText.find('/');
        const auto addressText = trim(entryText.substr(0, slash));
        const auto prefixText = slash == std::string::npos ? std::string() : trim(entryText.substr(slash + 1));

        const auto network = parseAddress(addressText);
        const auto prefix = slash == std::string::npos ? 32 : parsePrefix(prefixText);
        const auto mask = prefix == 0 ? 0U : (prefix == 32 ? 0xFFFFFFFFU : (~0U << (32 - prefix)));

        m_entries.push_back({network & mask, mask});

        if (comma == std::string::npos)
        {
            break;
        }
        begin = comma + 1;
    }
}

bool IpWhitelist::allows(const std::uint32_t addrNetworkByteOrder) const noexcept
{
    if (m_entries.empty())
    {
        return true;
    }
    const auto address = networkToHost(addrNetworkByteOrder);
    for (const auto& entry: m_entries)
    {
        if ((address & entry.mask) == entry.network)
        {
            return true;
        }
    }
    return false;
}

bool IpWhitelist::isValid(const std::string& text)
{
    try
    {
        IpWhitelist parsed(text);
        (void) parsed;
        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

} // namespace xmq