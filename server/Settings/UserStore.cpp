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


#include "UserStore.h"

#include "PasswordHash.h"
#include "service/CUsers.h"

#include <sptk5/JWT.h>

#include <filesystem>
#include <set>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// The tables, in the order they must be created: a child cannot reference a parent that is not
/// there yet, and SQLite with foreign keys on enforces that at creation time.
constexpr string_view userGroupTable = "xmq_user_group";
constexpr string_view userTable = "xmq_user";
constexpr string_view membershipTable = "xmq_user_to_user_group";
constexpr string_view versionTable = "xmq_store_version";

/// UTC, ISO, milliseconds - fixed width, so ordering the text orders the times. The gmt argument
/// defaults to false, which would write local time with an offset and sort wrongly against another
/// machine's, so it is passed here and formatted in this one place.
String nowUtc()
{
    return DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true);
}

/**
 * @brief The clear-text password inside an old xmq_users.conf entry.
 *
 * Kept as a JWT whose payload holds the password in the open - HS256 signs it, nothing encrypts
 * it. That is the whole reason the migration can produce real verifiers without asking anybody to
 * choose a new password, and the reason it had to stop being how passwords are stored.
 *
 * @return The password, or nothing if this entry cannot be read as one.
 */
std::optional<String> legacyPasswordOf(const String& stored)
{
    // The key the old format signs with. It never protected the password - the payload is readable
    // by anyone with the file - so there is nothing here that was not already in the file.
    static const String legacyKey("Error: The encryption key is not defined");

    try
    {
        JWT jwt;
        jwt.set_alg(JWT::Algorithm::HS256, legacyKey);
        jwt.decode(stored.c_str(), legacyKey);
        return String(jwt.get("password").asString());
    }
    catch (const Exception&)
    {
        return {};
    }
}

} // namespace

UserStore::UserStore(shared_ptr<DatabaseConnectionPool> database)
    : m_database(std::move(database))
{
    if (!m_database)
    {
        throw Exception("UserStore needs a database");
    }
}

String UserStore::tableSuffix() const
{
    // InnoDB by name, never the server's default. MyISAM accepts a FOREIGN KEY clause and enforces
    // nothing whatever - the second way MySQL will take a constraint and quietly not keep it, after
    // the inline REFERENCES form - and which engine a server defaults to is not ours to assume.
    const auto driver = m_database->driverName().toLowerCase();
    if (driver.starts_with("mysql") || driver.starts_with("mariadb"))
    {
        return " ENGINE=InnoDB";
    }
    return "";
}

String UserStore::serialColumn() const
{
    const auto driver = m_database->driverName().toLowerCase();

    if (driver.starts_with("sqlite"))
    {
        // AUTOINCREMENT, and not the bare rowid alias. Without it SQLite hands the id of a deleted
        // row to the next insert, and these ids are what the membership table points at: delete a
        // group and add another, and yesterday's members are in today's group. The keyword costs a
        // counter row and forbids reuse.
        return "integer primary key autoincrement";
    }
    if (driver.starts_with("postgres"))
    {
        return "serial primary key";
    }
    if (driver.starts_with("mysql") || driver.starts_with("mariadb"))
    {
        // Not the SERIAL that MySQL also understands. SERIAL is BIGINT UNSIGNED, and MySQL requires
        // a foreign key's column to match the one it references in type and signedness - so taking
        // it would force the membership columns to BIGINT UNSIGNED, which PostgreSQL does not have.
        // The dialect difference would move from the key to the keys pointing at it, not disappear.
        return "int auto_increment primary key";
    }

    throw Exception("UserStore does not know how " + driver + " spells an auto-assigned key");
}

bool UserStore::tableExists(const String& tableName)
{
    // Asked by querying the table rather than by reading a catalogue: every one of the three names
    // its catalogue differently, and a query that fails is the same answer with no dialect in it.
    try
    {
        const AutoDatabaseConnection connection(*m_database);
        Query                        probe(connection.connection(), "SELECT count(*) FROM " + tableName);
        probe.open();
        probe.close();
        return true;
    }
    catch (const Exception&)
    {
        return false;
    }
}

void UserStore::executeStatement(const String& sql)
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        statement(connection.connection(), sql);
    statement.exec();
}

void UserStore::createSchema()
{
    const auto serial = serialColumn();

    if (!tableExists(String(string(userGroupTable))))
    {
        executeStatement(
            "CREATE TABLE " + String(string(userGroupTable)) + " ("
            "  id         " + serial + ","
            "  name       varchar(80) NOT NULL,"
            "  revision   int NOT NULL DEFAULT 1,"
            "  updated_at varchar(40),"
            "  updated_by varchar(80),"
            "  CONSTRAINT uq_xmq_user_group_name UNIQUE (name))" + tableSuffix());
    }

    if (!tableExists(String(string(userTable))))
    {
        executeStatement(
            "CREATE TABLE " + String(string(userTable)) + " ("
            "  id         " + serial + ","
            "  username   varchar(80) NOT NULL,"
            "  password   varchar(255) NOT NULL,"
            "  is_enabled int NOT NULL DEFAULT 1,"
            "  revision   int NOT NULL DEFAULT 1,"
            "  updated_at varchar(40),"
            "  updated_by varchar(80),"
            "  CONSTRAINT uq_xmq_user_username UNIQUE (username))" + tableSuffix());
    }

    if (!tableExists(String(string(membershipTable))))
    {
        // Table-level FOREIGN KEY, never the REFERENCES clause inside a column definition. MySQL
        // parses the inline form and silently discards it - CREATE TABLE succeeds, without a
        // warning, and the table has no foreign key at all. Proven against MySQL 8.4 here: the
        // inline form produced a table with no constraint, the form below produced the constraint.
        //
        // ON DELETE CASCADE, so that removing an account or a group cannot leave a membership row
        // pointing at nothing - which would otherwise be read as a member of a group that no
        // longer exists, and permissions are not a place for rows nobody can account for.
        executeStatement(
            "CREATE TABLE " + String(string(membershipTable)) + " ("
            "  user_id       int NOT NULL,"
            "  user_group_id int NOT NULL,"
            "  CONSTRAINT pk_xmq_user_to_user_group PRIMARY KEY (user_id, user_group_id),"
            "  CONSTRAINT fk_xmq_membership_user FOREIGN KEY (user_id)"
            "      REFERENCES " + String(string(userTable)) + " (id) ON DELETE CASCADE,"
            "  CONSTRAINT fk_xmq_membership_group FOREIGN KEY (user_group_id)"
            "      REFERENCES " + String(string(userGroupTable)) + " (id) ON DELETE CASCADE)" + tableSuffix());

        // Membership is read both ways: everything one user belongs to, and everyone in one group.
        // The primary key already serves the first; this serves the second.
        executeStatement(
            "CREATE INDEX ix_user_to_group_by_group ON " + String(string(membershipTable)) + " (user_group_id)");
    }

    if (!tableExists(String(string(versionTable))))
    {
        executeStatement(
            "CREATE TABLE " + String(string(versionTable)) + " ("
            "  version    int NOT NULL,"
            "  updated_at varchar(40))" + tableSuffix());

        executeStatement("INSERT INTO " + String(string(versionTable)) + " (version) VALUES (1)");
    }

    restrictSqliteFile();
}

void UserStore::restrictSqliteFile() const
{
    if (!m_database->driverName().toLowerCase().starts_with("sqlite"))
    {
        return; // a server's file is the server's business, and there is no path here to change
    }

    // SQLite makes the file with whatever the umask allows, which on a service started by the
    // system is world-readable - and this file holds every account's password hash. The package
    // sets the permissions on the copy it installs; nothing set them on the one the broker makes
    // for itself on a first start, which is now every installation, because the package no longer
    // ships a live one.
    //
    // databaseName() and schema() together, the way the SQLite driver itself opens the file: the
    // connection string splits a path at its last separator, so databaseName() alone is the
    // directory the store lives in. Taking it for the file made the first attempt at this take the
    // execute bit off /etc/xmq instead - which locks out every account but the broker's own,
    // including the one meant to read the file it was protecting.
    const auto                  schema = m_database->schema();
    const std::string           databaseName(m_database->databaseName().c_str());
    const std::filesystem::path file(schema.empty() ? databaseName
                                                    : databaseName + "/" + std::string(schema.c_str()));
    std::error_code             errorCode;
    if (!std::filesystem::exists(file, errorCode))
    {
        return;
    }
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read,
                                 errorCode);
}

int64_t UserStore::version()
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        query(connection.connection(), "SELECT version FROM " + String(string(versionTable)));

    query.open();
    const auto version = query.eof() ? 0 : query[uint32_t {0}].asInt64();
    query.close();

    return version;
}

int64_t UserStore::addUser(const User& user)
{
    const AutoDatabaseConnection connection(*m_database);

    Query insert(connection.connection(),
                 "INSERT INTO " + String(string(userTable)) +
                 " (username, password, is_enabled, revision, updated_at, updated_by)"
                 " VALUES (:username, :password, :is_enabled, 1, :updated_at, :updated_by)");

    insert.param("username") = user.m_username;
    insert.param("password") = user.m_password;
    insert.param("is_enabled") = user.m_enabled ? 1 : 0;
    insert.param("updated_at") = user.m_updatedAt.empty() ? nowUtc() : user.m_updatedAt;
    insert.param("updated_by") = user.m_updatedBy;
    insert.exec();

    const auto added = findUser(user.m_username);
    return added ? added->m_id : 0;
}

namespace {

constexpr string_view userColumns =
    "id, username, password, is_enabled, revision, updated_at, updated_by";

UserStore::User readUser(Query& query)
{
    return UserStore::User {.m_id = query["id"].asInt64(),
                            .m_username = query["username"].asString(),
                            .m_password = query["password"].asString(),
                            .m_enabled = query["is_enabled"].asInteger() != 0,
                            .m_revision = query["revision"].asInteger(),
                            .m_updatedAt = query["updated_at"].asString(),
                            .m_updatedBy = query["updated_by"].asString()};
}

UserStore::Group readGroup(Query& query)
{
    return UserStore::Group {.m_id = query["id"].asInt64(),
                             .m_name = query["name"].asString(),
                             .m_revision = query["revision"].asInteger(),
                             .m_updatedAt = query["updated_at"].asString(),
                             .m_updatedBy = query["updated_by"].asString()};
}

} // namespace

std::optional<UserStore::User> UserStore::findUser(const String& username)
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT " + String(string(userColumns)) +
                " FROM " + String(string(userTable)) + " WHERE username = :username");
    query.param("username") = username;
    query.open();

    std::optional<User> user;
    if (!query.eof())
    {
        user = readUser(query);
    }
    query.close();

    return user;
}

std::vector<UserStore::User> UserStore::users()
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT " + String(string(userColumns)) +
                " FROM " + String(string(userTable)) + " ORDER BY username");
    query.open();

    std::vector<User> users;
    while (!query.eof())
    {
        users.push_back(readUser(query));
        query.fetch();
    }
    query.close();

    return users;
}

namespace {

/// The accounts an old users file holds, or none when there is no such file.
std::optional<CUsers> loadLegacyUsers(const std::filesystem::path& usersFile)
{
    std::error_code errorCode;
    if (!std::filesystem::exists(usersFile, errorCode))
    {
        // A fresh installation, which is the ordinary case and not a failure.
        return {};
    }

    Buffer contents;
    contents.loadFromFile(usersFile);

    xdoc::Document document;
    document.load(String(contents));

    CUsers users;
    users.load(document.root());
    return users;
}

/**
 * @brief The accounts an old users file marked as administrators.
 *
 * Read from the document rather than through CUser, which no longer has the field: being an
 * administrator is a membership of the Administrators group now. The old file is yesterday's
 * format, and reading it means reading what yesterday wrote - including a field today's type has
 * no place for.
 */
std::set<String> legacyAdministrators(const std::filesystem::path& usersFile)
{
    std::set<String> administrators;

    std::error_code errorCode;
    if (!std::filesystem::exists(usersFile, errorCode))
    {
        return administrators;
    }

    Buffer contents;
    contents.loadFromFile(usersFile);

    xdoc::Document document;
    document.load(String(contents));

    const auto users = document.root()->findFirst("users");
    if (users == nullptr)
    {
        return administrators;
    }

    for (const auto& node: users->nodes())
    {
        const auto flag = node->findFirst("is_admin");
        const auto name = node->findFirst("username");
        if (flag != nullptr && name != nullptr && flag->getBoolean())
        {
            administrators.insert(String(name->getString()));
        }
    }

    return administrators;
}

} // namespace

std::optional<String> UserStore::legacyPasswordFrom(const std::filesystem::path& usersFile,
                                                    const String&                username)
{
    const auto users = loadLegacyUsers(usersFile);
    if (!users)
    {
        return {};
    }

    for (const auto& user: users->m_users)
    {
        if (String(user.m_username.asString()) == username)
        {
            return legacyPasswordOf(String(user.m_password.asString()));
        }
    }
    return {};
}

std::pair<int64_t, int64_t> UserStore::createStandardGroups()
{
    const auto ensure = [this](const String& name)
    {
        if (const auto existing = findGroup(name))
        {
            return existing->m_id;
        }
        return addGroup({.m_name = name, .m_updatedBy = "installation"});
    };

    return {ensure(String(string(defaultGroupName))), ensure(String(string(administratorsGroupName)))};
}

size_t UserStore::importUsersFrom(const std::filesystem::path& usersFile, const int iterations)
{
    const auto loaded = loadLegacyUsers(usersFile);
    if (!loaded)
    {
        return 0;
    }
    const auto& users = *loaded;

    // Every account joins Default, and the administrators join Administrators as well: the
    // membership is then already right when permissions arrive, rather than being retro-fitted to
    // accounts nobody remembers the shape of.
    const auto [defaultGroup, administratorsGroup] = createStandardGroups();
    const auto administrators = legacyAdministrators(usersFile);

    size_t imported = 0;
    for (const auto& user: users.m_users)
    {
        const String username(user.m_username.asString());
        if (username.empty() || findUser(username))
        {
            // Already here: a migration that ran once already, or an account an administrator has
            // since edited. Either way this file is not the authority any more.
            continue;
        }

        // Both forms are accepted, so that it does not matter whether this runs before or after
        // the accounts in the file have been converted: a verifier moves across as it is, and the
        // old JWT is converted here, on a reading that can still see the password.
        const String stored(user.m_password.asString());

        String verifier;
        if (stored.empty())
        {
            // The account whose password has never been set - the first-start administrator. It
            // migrates with none, which is the state that opens the setup door, rather than with a
            // verifier for the empty string, which would let an empty password through that door.
        }
        else if (PasswordHash::isHashed(stored))
        {
            verifier = stored;
        }
        else if (const auto password = legacyPasswordOf(stored); password && !password->empty())
        {
            verifier = PasswordHash::hash(*password, iterations);
        }
        else if (password)
        {
            // A readable password that is empty: the same never-set state, written in the older
            // form.
        }
        else
        {
            // Unreadable rather than absent. Skipped and said out loud: inventing a password would
            // create an account nobody can use, and refusing to start would strand the migration.
            CERR("Cannot read the stored password of '" << username << "', so it is not migrated");
            continue;
        }

        const auto isAdministrator = administrators.contains(username);

        addUser(User {.m_username = username,
                      .m_password = verifier,
                      .m_enabled = user.m_is_enabled.isNull() || user.m_is_enabled.asBool(),
                      .m_updatedBy = "migration"});

        if (const auto added = findUser(username))
        {
            addUserToGroup(added->m_id, defaultGroup);
            if (isAdministrator)
            {
                addUserToGroup(added->m_id, administratorsGroup);
            }
        }
        ++imported;
    }

    return imported;
}

void UserStore::updateUser(const User& user)
{
    const AutoDatabaseConnection connection(*m_database);

    Query update(connection.connection(),
                 "UPDATE " + String(string(userTable)) +
                 " SET password = :password, is_enabled = :is_enabled,"
                 "     revision = revision + 1, updated_at = :updated_at, updated_by = :updated_by"
                 " WHERE username = :username");

    update.param("username") = user.m_username;
    update.param("password") = user.m_password;
    update.param("is_enabled") = user.m_enabled ? 1 : 0;
    update.param("updated_at") = user.m_updatedAt.empty() ? nowUtc() : user.m_updatedAt;
    update.param("updated_by") = user.m_updatedBy;
    update.exec();
}

void UserStore::removeUser(const int64_t userId)
{
    // Memberships go with it, by the cascade the schema declares - which is enforced, not merely
    // written down: see the test that inserts a membership for accounts that do not exist.
    const AutoDatabaseConnection connection(*m_database);
    Query                        remove(connection.connection(),
                                        "DELETE FROM " + String(string(userTable)) + " WHERE id = :id");
    remove.param("id") = userId;
    remove.exec();
}

int64_t UserStore::addGroup(const Group& group)
{
    const AutoDatabaseConnection connection(*m_database);

    Query insert(connection.connection(),
                 "INSERT INTO " + String(string(userGroupTable)) +
                 " (name, revision, updated_at, updated_by) VALUES (:name, 1, :updated_at, :updated_by)");
    insert.param("name") = group.m_name;
    insert.param("updated_at") = group.m_updatedAt.empty() ? nowUtc() : group.m_updatedAt;
    insert.param("updated_by") = group.m_updatedBy;
    insert.exec();

    const auto added = findGroup(group.m_name);
    return added ? added->m_id : 0;
}

std::optional<UserStore::Group> UserStore::findGroup(const String& name)
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT id, name, revision, updated_at, updated_by FROM " +
                String(string(userGroupTable)) + " WHERE name = :name");
    query.param("name") = name;
    query.open();

    std::optional<Group> group;
    if (!query.eof())
    {
        group = readGroup(query);
    }
    query.close();

    return group;
}

std::vector<UserStore::Group> UserStore::groups()
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT id, name, revision, updated_at, updated_by FROM " +
                String(string(userGroupTable)) + " ORDER BY name");
    query.open();

    std::vector<Group> groups;
    while (!query.eof())
    {
        groups.push_back(readGroup(query));
        query.fetch();
    }
    query.close();

    return groups;
}

void UserStore::removeGroup(const int64_t groupId)
{
    const AutoDatabaseConnection connection(*m_database);
    Query                        remove(connection.connection(),
                                        "DELETE FROM " + String(string(userGroupTable)) + " WHERE id = :id");
    remove.param("id") = groupId;
    remove.exec();
}

void UserStore::addUserToGroup(const int64_t userId, const int64_t groupId)
{
    const AutoDatabaseConnection connection(*m_database);

    // Asked first rather than caught afterwards. The primary key would refuse a repeat, but the
    // three databases word that refusal differently and telling it apart from a real failure by
    // its message is how a store starts swallowing errors it should not.
    Query existing(connection.connection(),
                   "SELECT count(*) FROM " + String(string(membershipTable)) +
                   " WHERE user_id = :user_id AND user_group_id = :group_id");
    existing.param("user_id") = userId;
    existing.param("group_id") = groupId;
    existing.open();
    const auto alreadyThere = !existing.eof() && existing[uint32_t {0}].asInteger() > 0;
    existing.close();

    if (alreadyThere)
    {
        return;
    }

    Query insert(connection.connection(),
                 "INSERT INTO " + String(string(membershipTable)) +
                 " (user_id, user_group_id) VALUES (:user_id, :group_id)");
    insert.param("user_id") = userId;
    insert.param("group_id") = groupId;
    insert.exec();
}

void UserStore::removeUserFromGroup(const int64_t userId, const int64_t groupId)
{
    const AutoDatabaseConnection connection(*m_database);

    Query remove(connection.connection(),
                 "DELETE FROM " + String(string(membershipTable)) +
                 " WHERE user_id = :user_id AND user_group_id = :group_id");
    remove.param("user_id") = userId;
    remove.param("group_id") = groupId;
    remove.exec();
}

std::vector<UserStore::Group> UserStore::groupsOfUser(const int64_t userId)
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT g.id, g.name, g.revision, g.updated_at, g.updated_by"
                " FROM " + String(string(userGroupTable)) + " g"
                " JOIN " + String(string(membershipTable)) + " m ON m.user_group_id = g.id"
                " WHERE m.user_id = :user_id ORDER BY g.name");
    query.param("user_id") = userId;
    query.open();

    std::vector<Group> groups;
    while (!query.eof())
    {
        groups.push_back(readGroup(query));
        query.fetch();
    }
    query.close();

    return groups;
}

std::vector<UserStore::User> UserStore::usersInGroup(const int64_t groupId)
{
    const AutoDatabaseConnection connection(*m_database);

    Query query(connection.connection(),
                "SELECT u.id, u.username, u.password, u.is_enabled, u.revision,"
                "       u.updated_at, u.updated_by"
                " FROM " + String(string(userTable)) + " u"
                " JOIN " + String(string(membershipTable)) + " m ON m.user_id = u.id"
                " WHERE m.user_group_id = :group_id ORDER BY u.username");
    query.param("group_id") = groupId;
    query.open();

    std::vector<User> users;
    while (!query.eof())
    {
        users.push_back(readUser(query));
        query.fetch();
    }
    query.close();

    return users;
}
