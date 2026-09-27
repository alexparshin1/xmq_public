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

#include <sptk5/cdatabase>
#include <sptk5/cutils>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xmq {

/**
 * @brief Accounts and groups, in an SQL database.
 *
 * Replaces the JSON users file. SQLite, PostgreSQL and MySQL are all supported, and the only thing
 * that differs between them is how a table declares its own key - so the schema below is written
 * once and only that clause is chosen per driver.
 *
 * Nothing here caches. The broker's own cache is filled from a notification, in one place, so that
 * a change that will not apply fails identically everywhere rather than on some nodes only; this
 * class is what that one place reads through.
 */
class XMQ_EXPORT UserStore
{
public:
    /// One account, as the database holds it.
    struct User
    {
        int64_t       m_id {0};
        sptk::String  m_username;
        /// The stored verifier, never a password. See PasswordHash.
        sptk::String  m_password;
        bool          m_enabled {true};
        int           m_revision {1};
        sptk::String  m_updatedAt;
        sptk::String  m_updatedBy;
    };

    /// One group. Its permissions arrive in a later version; the table is here because membership
    /// is, and a group with no rules yet is still a group somebody has been put in.
    struct Group
    {
        int64_t      m_id {0};
        sptk::String m_name;
        int          m_revision {1};
        sptk::String m_updatedAt;
        sptk::String m_updatedBy;
    };

    explicit UserStore(std::shared_ptr<sptk::DatabaseConnectionPool> database);

    /**
     * @brief Creates the tables if they are not there, and does nothing if they are.
     *
     * Safe to call at every start, which is how it is called: the alternative is a separate
     * migration step that somebody has to remember to run, and a broker that will not start
     * because of it.
     */
    void createSchema();

    /**
     * @brief Take the group and world bits off a SQLite store this broker created.
     *
     * Only for SQLite, and only for the file this store points at: it holds password hashes, and
     * SQLite makes it with whatever the umask allows. Does nothing for a server-hosted database,
     * whose permissions are that server's business.
     */
    void restrictSqliteFile() const;

    /// The version of the whole store, raised by every change. A node re-reads when this moves.
    [[nodiscard]] int64_t version();

    /**
     * @brief Adds one account.
     * @param user      Its password field must already be a verifier, never a password.
     * @return The id the database assigned.
     */
    int64_t addUser(const User& user);

    /// One account by name, or nothing if there is no such account.
    [[nodiscard]] std::optional<User> findUser(const sptk::String& username);

    /// Every account, for the interface and for the tests.
    [[nodiscard]] std::vector<User> users();

    /**
     * @brief Changes an existing account, found by its username.
     * @remarks Raises its revision, which is what tells one version of a row from another when two
     *          nodes edit at once and when clocks disagree - see updated_at, which answers "when"
     *          for a person reading the table rather than "which" for the code.
     */
    void updateUser(const User& user);

    /// Removes an account, and with it every membership it had.
    void removeUser(int64_t userId);

    /**
     * @brief Adds a group.
     * @return The id the database assigned.
     */
    int64_t addGroup(const Group& group);

    /// One group by name, or nothing if there is no such group.
    [[nodiscard]] std::optional<Group> findGroup(const sptk::String& name);

    /// Every group, by name.
    [[nodiscard]] std::vector<Group> groups();

    /// Removes a group, and with it every membership of it. The accounts themselves stay.
    void removeGroup(int64_t groupId);

    /**
     * @brief Puts an account in a group, or leaves it there if it already is.
     * @remarks Idempotent on purpose: an interface that sends the whole membership of an account
     *          on every save would otherwise fail on the rows that had not changed.
     */
    void addUserToGroup(int64_t userId, int64_t groupId);

    /// Takes an account out of a group. Doing it twice is not an error.
    void removeUserFromGroup(int64_t userId, int64_t groupId);

    /// The groups one account belongs to, by name.
    [[nodiscard]] std::vector<Group> groupsOfUser(int64_t userId);

    /// The accounts in one group, by username.
    [[nodiscard]] std::vector<User> usersInGroup(int64_t groupId);

    /**
     * @brief Brings the accounts of an xmq_users.conf into the database, if there is one.
     *
     * Done once, and it has to be done here rather than by hand afterwards, because the old file
     * holds each password in clear text inside a JWT - so this can compute a proper verifier from
     * it without asking anybody to choose a new password. After the migration only the verifier
     * remains and that chance is gone for good.
     *
     * Accounts already in the database are left alone: this is a migration, not a synchronisation,
     * and a second run must not undo an administrator's edits.
     *
     * @param usersFile     Path of the file. Missing is not an error - it means a fresh install.
     * @param iterations    Hashing cost for the verifiers it writes.
     * @return How many accounts were added.
     */
    size_t importUsersFrom(const std::filesystem::path& usersFile, int iterations);

    /**
     * @brief The clear-text password one account has in an old xmq_users.conf.
     *
     * For the one secret that has to survive the migration readable: the 'cluster' account, whose
     * password every node presents to its peers. It moves into the configuration, and the account
     * itself keeps only a verifier like every other. This is the last moment it can be read.
     *
     * @return The password, or nothing if the file or the account is missing or unreadable.
     */
    static std::optional<sptk::String> legacyPasswordFrom(const std::filesystem::path& usersFile,
                                                          const sptk::String&          username);

    /// The group every account belongs to, made when the store is first filled.
    static constexpr std::string_view defaultGroupName = "Default";

    /// The group that says who may administer the broker.
    static constexpr std::string_view administratorsGroupName = "Administrators";

    /**
     * @brief Creates the two groups every installation has, if they are not there.
     * @return The ids of the default group and the administrators group.
     */
    std::pair<int64_t, int64_t> createStandardGroups();

private:
    /// What this driver needs after the closing bracket of a CREATE TABLE, if anything.
    [[nodiscard]] sptk::String tableSuffix() const;

    /// How this driver spells "integer primary key that assigns itself".
    [[nodiscard]] sptk::String serialColumn() const;

    /// Whether the named table is already there, asked without relying on a dialect's catalogue.
    [[nodiscard]] bool tableExists(const sptk::String& tableName);

    void executeStatement(const sptk::String& sql);

    std::shared_ptr<sptk::DatabaseConnectionPool> m_database;
};

} // namespace xmq
