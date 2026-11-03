#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xmq {

/**
 * @brief Plan and apply an import of a Mosquitto password file.
 *
 * A line is `username:verifier`. Mosquitto 2.0 writes `$7$` PBKDF2-HMAC-SHA512 verifiers, which are
 * copied as they are: a password cannot be re-hashed without the password, and these can be checked
 * where they stand. An older `$6$` SHA512-crypt verifier and a 2.1 `$argon2id$` one are reported as
 * unsupported rather than stored as something they are not. A clear-text line, which Mosquitto also
 * accepts, is hashed with this broker's own verifier when the import is applied.
 */
class PasswordImport
{
public:
    enum class Status
    {
        New,
        Existing,
        Unsupported,
        Invalid
    };

    struct Entry
    {
        std::string m_username;
        std::string m_verifier;     ///< The stored verifier, or the clear password when m_plaintext.
        Status      m_status {Status::New};
        std::string m_reason;
        bool        m_plaintext {false};
    };

    struct Plan
    {
        std::vector<Entry> m_new;
        std::vector<Entry> m_existing;
        std::vector<Entry> m_unsupported;
        std::vector<Entry> m_invalid;
    };

    /// Read the lines of a password file. Nothing is written and no hash is calculated.
    static std::vector<Entry> parse(std::string_view content);

    /// Compare the parsed accounts with the ones that already exist, by username.
    static Plan plan(const std::vector<Entry>& entries, const std::set<std::string>& existingUsernames);

    /// The plan as JSON for the control API and the import screen. Never contains a verifier.
    static std::string toJson(const Plan& plan);
};

} // namespace xmq
