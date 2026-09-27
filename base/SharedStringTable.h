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

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace xmq {

/**
 * @brief One copy of a string that many objects name.
 *
 * A broker holds a session per connection, and several of the strings in a session are the same
 * string over and over: the account a fleet of devices signs in as, the name of the node, the bridge
 * a session came in through. Held one per session, each of those is a 32-byte std::string and a heap
 * block whenever it does not fit inline. Held here, it is one copy and an 8-byte pointer apiece.
 *
 * The rules are the ones the topic table already follows, and they are what make a bare pointer safe:
 * an entry is never changed after it is made, and an entry is never removed. The set is node-based,
 * so a rehash moves no strings; the pointer handed out stays good for the life of the process.
 *
 * Not for strings a client can invent without limit - a client id, a topic - because nothing here is
 * ever given back. Names are what this is for: their number is bounded by what an installation is
 * configured with, and the table costs a few tens of bytes for each one, once.
 */
class SharedStringTable
{
public:
    /**
     * @return The process-wide table.
     */
    static SharedStringTable& instance()
    {
        static SharedStringTable table;
        return table;
    }

    /**
     * @brief Find or make the table's copy of a string.
     * @param text              The string to look up.
     * @return A pointer to the table's copy, or nullptr for an empty string - which is not worth an
     *         entry, and which callers read as "not set" anyway.
     */
    const std::string* intern(std::string_view text)
    {
        if (text.empty())
        {
            return nullptr;
        }

        {
            const std::shared_lock lock(m_mutex);
            if (const auto iterator = m_strings.find(text);
                iterator != m_strings.end())
            {
                return &*iterator;
            }
        }

        const std::unique_lock lock(m_mutex);
        return &*m_strings.emplace(text).first;
    }

    /**
     * @return How many distinct strings the table holds.
     */
    [[nodiscard]] size_t size() const
    {
        const std::shared_lock lock(m_mutex);
        return m_strings.size();
    }

private:
    SharedStringTable() = default;

    /// Transparent hashing, so that a lookup by string_view does not build a std::string first.
    struct Hash
    {
        using is_transparent = void;
        size_t operator()(std::string_view text) const noexcept
        {
            return std::hash<std::string_view> {}(text);
        }
    };

    struct Equal
    {
        using is_transparent = void;
        bool operator()(std::string_view left, std::string_view right) const noexcept
        {
            return left == right;
        }
    };

    mutable std::shared_mutex                       m_mutex;   ///< Readers share it; a new name is rare.
    std::unordered_set<std::string, Hash, Equal>    m_strings; ///< Node-based, so entries never move.
};

} // namespace xmq
