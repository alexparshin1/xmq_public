/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE — sample extension                   ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This sample is placed in the public domain, or under CC0 1.0 where that is   ║
║  not possible. Copy it into your own extension, closed or open, without       ║
║  attribution or obligation — it exists to be copied. Note that this differs   ║
║  from the broker itself, which is under the Mozilla Public License 2.0.       ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

/**
 * @file UserDatabase.cpp
 * @brief Authentication against the broker's SQL user database.
 *
 * This is the extension a standalone broker runs. It answers CONNECT from the same tables the
 * configuration interface edits - xmq_user and its groups - so an account added on the Users screen
 * can connect a second later, on any node sharing the database.
 *
 * It is also the worked example for an extension that has a store. Everything an LDAP or OIDC
 * extension has to get right is here and is the same: a connection made once and reused, a blocking
 * lookup on the broker's own thread, four answers rather than two, and an outage told apart from a
 * wrong password.
 *
 * ## The four answers
 *
 *  - ALLOW - the account exists, is enabled, and the password verifies.
 *  - DENY - the account exists and something about it says no.
 *  - SUBSYSTEM_ERROR - the database could not be reached. Not DENY, which would tell a client with
 *    perfectly good credentials that they are wrong, and not NOT_HANDLED, which would hand them to
 *    whoever is next and make an outage look like somebody else's user.
 *  - NOT_HANDLED - only for a client with no username, which is the broker's anonymous policy to
 *    decide, and for an unknown account when the configuration asks for that.
 *
 * ## Why the password check is a copy
 *
 * The verifier below reads `pbkdf2-sha256$iterations$salt$hash`, which is what the broker writes.
 * It is deliberately a second implementation: an extension includes no broker header, and that
 * boundary is worth more than the thirty lines saved. What keeps the two honest is that the tests
 * hash with the broker's own code and verify with this one, so a change to either format is a
 * failing test rather than a broker that stops admitting anybody.
 *
 * ## What it does not do
 *
 * No groups and no permissions: authorisation is a separate capability and a later version. The
 * account's group membership is in the database and is read by the authorizer, not by this.
 */

#include <extension/XmqExtension.h>

#include <sptk5/db/AutoDatabaseConnection.h>
#include <sptk5/db/DatabaseConnectionPool.h>
#include <sptk5/db/DatabaseConnectionString.h>
#include <sptk5/db/Query.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace {

constexpr std::string_view algorithmName = "pbkdf2-sha256";

/// One connection per authentication thread the broker keeps, and no more: the pool is not the
/// place to discover that a database has a connection limit.
constexpr size_t connections = 4;

/// Hex to bytes, refusing anything that is not hex rather than reading it as zero - a verifier with
/// a damaged salt must fail to verify, not verify against the wrong salt.
bool fromHex(const std::string_view text, std::vector<unsigned char>& bytes)
{
    if (text.size() % 2 != 0)
    {
        return false;
    }

    bytes.clear();
    bytes.reserve(text.size() / 2);
    for (size_t at = 0; at < text.size(); at += 2)
    {
        unsigned  value = 0;
        const auto result = std::from_chars(text.data() + at, text.data() + at + 2, value, 16);
        if (result.ec != std::errc() || result.ptr != text.data() + at + 2)
        {
            return false;
        }
        bytes.push_back(static_cast<unsigned char>(value));
    }
    return true;
}

/// Does this password produce this verifier? The other half of the broker's PasswordHash::hash().
bool verify(const std::string& password, const std::string& stored)
{
    const std::string_view view(stored);

    const auto first = view.find('$');
    const auto second = first == std::string_view::npos ? first : view.find('$', first + 1);
    const auto third = second == std::string_view::npos ? second : view.find('$', second + 1);
    if (third == std::string_view::npos)
    {
        return false;
    }

    if (view.substr(0, first) != algorithmName)
    {
        return false;
    }

    int        iterations = 0;
    const auto iterationText = view.substr(first + 1, second - first - 1);
    if (std::from_chars(iterationText.data(), iterationText.data() + iterationText.size(), iterations).ec !=
            std::errc() ||
        iterations < 1)
    {
        return false;
    }

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!fromHex(view.substr(second + 1, third - second - 1), salt) ||
        !fromHex(view.substr(third + 1), expected) || expected.empty())
    {
        return false;
    }

    std::vector<unsigned char> derived(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          static_cast<int>(derived.size()), derived.data()) != 1)
    {
        return false;
    }

    // Constant-time: the number of leading bytes that matched is not something a client should be
    // able to measure.
    return CRYPTO_memcmp(derived.data(), expected.data(), expected.size()) == 0;
}

/// How this database spells a key it assigns itself. The three the broker's own store supports are
/// the three worth knowing here; anything else is a database this extension has not been tried
/// against, and saying so beats guessing at its dialect.
std::string serialColumn(const std::string& driver, std::string& problem)
{
    if (driver.starts_with("sqlite"))
    {
        // AUTOINCREMENT rather than the bare rowid alias: SQLite otherwise hands the id of a
        // deleted row to the next insert, and an id that comes back is an id something else may
        // still be pointing at.
        return "integer primary key autoincrement";
    }
    if (driver.starts_with("postgres"))
    {
        return "serial primary key";
    }
    if (driver.starts_with("mysql") || driver.starts_with("mariadb"))
    {
        return "int auto_increment primary key";
    }

    problem = "This extension does not know how " + driver +
              " spells an auto-assigned key, so it cannot create its table there."
              "\n\nCreate xmq_user by hand, or use SQLite, PostgreSQL, MySQL or MariaDB.";
    return {};
}

std::string tableSuffix(const std::string& driver)
{
    return driver.starts_with("mysql") || driver.starts_with("mariadb") ? " ENGINE=InnoDB" : "";
}

/**
 * @brief Remembers what verifying a password answered, so the cost is paid once.
 *
 * Without this the extension cannot carry the CONNECT path at all. PBKDF2-HMAC-SHA256 at 10000
 * iterations is about 1.13 ms, and the broker hands every connection to an authenticator over a
 * pool of four threads: some 3500 connections a second, and a burst larger than the queue behind
 * them is refused outright. That is what a scenario run does in its first second.
 *
 * Refusals are kept as well as admissions. Otherwise a client sending wrong passwords costs a full
 * KDF each time, which is a way to spend the broker's cores from outside. A corrected password
 * hashes to a different entry and is verified properly, so keeping refusals delays no repair.
 *
 * Nothing here holds a password. An entry is found by an HMAC of the credentials under a key made
 * when the process starts and never written anywhere, so a core file yields nothing replayable.
 *
 * Direct-mapped and fixed in size: a collision overwrites, and a miss is not a failure - it falls
 * through to a real verification, which is correct and merely slower.
 *
 * **An entry lives 60 seconds and there is nothing that can cut it short.** The broker's own cache
 * has the same lifetime but also forgets everything the moment an account changes; an extension is
 * not told when the Users screen edits one. So a password changed or an account disabled there
 * keeps working here for up to a minute. That is the cost of the cache, and it is why the lifetime
 * is a minute rather than an hour.
 */
class VerificationCache
{
public:
    VerificationCache()
    {
        if (RAND_bytes(m_key.data(), static_cast<int>(m_key.size())) != 1)
        {
            // Without a key there is nothing safe to index by, so the cache stays off and every
            // verification is done properly. Slower, never wrong.
            m_usable = false;
        }
    }

    enum class Answer
    {
        Unknown,
        Allowed,
        Refused
    };

    Answer look(const std::string& username, const std::string& password) const
    {
        if (!m_usable)
        {
            return Answer::Unknown;
        }

        const auto digest = tag(username, password);
        const std::shared_lock lock(m_mutex);

        const auto& slot = m_slots[index(digest)];
        if (slot.m_expiresAtMs <= nowMs() || slot.m_digest != digest)
        {
            return Answer::Unknown;
        }
        return slot.m_allowed ? Answer::Allowed : Answer::Refused;
    }

    void remember(const std::string& username, const std::string& password, const bool allowed)
    {
        if (!m_usable)
        {
            return;
        }

        const auto digest = tag(username, password);
        const std::scoped_lock lock(m_mutex);

        auto& slot = m_slots[index(digest)];
        slot.m_digest = digest;
        slot.m_allowed = allowed;
        slot.m_expiresAtMs = nowMs() + lifetimeMs;
    }

    /// Everything, because the store itself has been replaced.
    void forget()
    {
        const std::scoped_lock lock(m_mutex);
        for (auto& slot: m_slots)
        {
            slot.m_expiresAtMs = 0;
        }
    }

private:
    using Digest = std::array<unsigned char, 32>;

    struct Slot
    {
        Digest  m_digest {};
        int64_t m_expiresAtMs {};
        bool    m_allowed {false};
    };

    /// Steady, not wall: a lifetime is a duration, and must not be lengthened or cut short by
    /// somebody correcting the clock.
    static int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    Digest tag(const std::string& username, const std::string& password) const
    {
        // The separator matters: without it ("ab", "c") and ("a", "bc") are the same bytes, and one
        // account's password would answer for another's.
        std::string material;
        material.reserve(username.size() + password.size() + 1);
        material.append(username);
        material.push_back('\0');
        material.append(password);

        Digest       digest {};
        unsigned int length = 0;
        HMAC(EVP_sha256(), m_key.data(), static_cast<int>(m_key.size()),
             reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest.data(),
             &length);
        return digest;
    }

    static size_t index(const Digest& digest)
    {
        size_t value = 0;
        for (size_t at = 0; at < sizeof(value); ++at)
        {
            value = (value << 8U) | digest[at];
        }
        return value % slotCount;
    }

    static constexpr size_t  slotCount = 4096;
    static constexpr int64_t lifetimeMs = 60000;

    mutable std::shared_mutex        m_mutex;
    std::array<Slot, slotCount>      m_slots {};
    std::array<unsigned char, 32>    m_key {};
    bool                             m_usable {true};
};

class UserDatabase : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    /**
     * @brief Opens a pool for these settings and proves it can actually be used.
     *
     * Returns the pool, or nullptr with `problem` filled in as something an administrator can act
     * on. Connecting is not enough to call a configuration good: a URI can name a server that
     * answers and a database that has never had the broker's tables put in it, and the two faults
     * need different repairs. So the probe is a real query against the real table.
     */
    std::shared_ptr<sptk::DatabaseConnectionPool> openFor(const std::string& uri,
                                                          const std::string& username,
                                                          const std::string& password,
                                                          std::string&       problem,
                                                          std::string&       shown,
                                                          int64_t&           accounts,
                                                          bool&              created) const
    {
        problem.clear();
        created = false;

        if (uri.empty())
        {
            // The broker gives the address; empty means it keeps its accounts somewhere this
            // extension cannot read, or it is older than the ABI that answers the question.
            problem = "The broker reports no account database, so there is nobody this could "
                      "authenticate.";
            return nullptr;
        }

        // The credentials are settings of their own rather than part of the URI, because only the
        // password is a secret: masking the whole URI would hide which server and which database
        // this is authenticating against, which is what somebody reading this screen came for.
        sptk::DatabaseConnectionString connection(uri.c_str());
        if (!username.empty())
        {
            connection.userName(username.c_str());
        }
        if (!password.empty())
        {
            connection.password(password.c_str());
        }

        // Without the password, and used for every message this extension writes about the
        // database. A connection string reaches a log the moment something goes wrong with it.
        shown = std::string(connection.toString(false));

        std::shared_ptr<sptk::DatabaseConnectionPool> pool;
        try
        {
            pool = std::make_shared<sptk::DatabaseConnectionPool>(connection.toString(true), static_cast<unsigned>(connections));
        }
        catch (const std::exception& exception)
        {
            // Three blocks with a blank line between: what failed, what the database said, what
            // to do about it. Run together they are a paragraph nobody finishes reading, and the
            // part that matters - the last one - is the part that gets skipped.
            problem = "Cannot open " + shown + ".\n\nThe database said: " +
                      std::string(exception.what()) +
                      "\n\nFor PostgreSQL and MySQL the database and the database account must "
                      "already exist. Only the tables in it are this extension's to create.";
            return nullptr;
        }

        // Asked by querying the table rather than by reading a catalogue: each of the three names
        // its catalogue differently, and a query that fails is the same answer with no dialect in
        // it. This is also the connection test - a server that is not there fails here.
        int64_t held = 0;
        std::string firstReason;
        if (!countAccounts(pool, held, firstReason))
        {
            if (!createTable(pool, std::string(connection.driverName().toLowerCase()), problem))
            {
                if (problem.empty())
                {
                    // The table is not there and creating it failed for a reason that is not about
                    // the table: the server never answered. Report that, not the missing table.
                    problem = "Cannot use " + shown + ".\n\nThe database said: " + firstReason +
                              "\n\nFor PostgreSQL and MySQL the database and the database account "
                              "must already exist. Only the tables in it are this extension's to "
                              "create.";
                }
                return nullptr;
            }

            if (!countAccounts(pool, held, problem))
            {
                problem = "Created xmq_user in " + shown +
                          " but still cannot read it.\n\nThe database said: " + problem;
                return nullptr;
            }

            // Said plainly, because an empty directory admits nobody and looks from the outside
            // exactly like a broken one. This extension does not invent an account to put in it:
            // a shipped password is a password everybody knows.
            created = true;
        }

        accounts = held;
        return pool;
    }

    /// Counts the accounts, or says why it could not. False means the table is not readable -
    /// which is the same answer for "no such table" and "no such server", told apart by the caller.
    static bool countAccounts(const std::shared_ptr<sptk::DatabaseConnectionPool>& pool,
                              int64_t& accounts, std::string& reason)
    {
        try
        {
            const sptk::AutoDatabaseConnection held(*pool);
            sptk::Query                        probe(held.connection(),
                                                     "SELECT count(*) AS accounts FROM xmq_user");
            probe.open();
            accounts = probe["accounts"].asInteger();
            probe.close();
            return true;
        }
        catch (const std::exception& exception)
        {
            reason = exception.what();
            return false;
        }
    }

    /// Creates this extension's own table. Its directory, its schema - the same reason an LDAP
    /// extension owns the directory it authenticates against and the broker never writes to it.
    ///
    /// The shape matches what the broker's own UserStore creates, so that the common configuration
    /// - this extension pointed at the database the broker already uses - finds the table already
    /// there and creates nothing. The broker creates its schema before extensions start, so in
    /// that configuration this never runs.
    static bool createTable(const std::shared_ptr<sptk::DatabaseConnectionPool>& pool,
                            const std::string& driver, std::string& problem)
    {
        problem.clear();
        const auto serial = serialColumn(driver, problem);
        if (serial.empty())
        {
            return false;
        }

        try
        {
            const sptk::AutoDatabaseConnection held(*pool);
            sptk::Query create(held.connection(),
                               "CREATE TABLE xmq_user ("
                               "  id         " + serial + ","
                               "  username   varchar(80) NOT NULL,"
                               "  password   varchar(255) NOT NULL,"
                               "  is_enabled int NOT NULL DEFAULT 1,"
                               "  revision   int NOT NULL DEFAULT 1,"
                               "  updated_at varchar(40),"
                               "  updated_by varchar(80),"
                               "  CONSTRAINT uq_xmq_user_username UNIQUE (username))" +
                                   tableSuffix(driver));
            create.exec();
            return true;
        }
        catch (const std::exception& exception)
        {
            if (problem.empty())
            {
                problem = "Could not create the xmq_user table.\n\nThe database said: " +
                          std::string(exception.what()) +
                          "\n\nThe database account this connects with has to be allowed to "
                          "create tables, or somebody has to create xmq_user by hand.";
            }
            return false;
        }
    }

    /**
     * @brief Copies the accounts of one store into another, and says how many.
     *
     * Only ever into a table this extension has just created, never into one that was already
     * there: a store somebody else populates - the shared one a cluster authenticates against -
     * must not quietly acquire this node's local accounts because a URI was edited.
     *
     * Ids are not copied. They are the new store's to assign, and the only thing pointing at them
     * is group membership, which this extension does not carry.
     */
    static bool importAccounts(const std::shared_ptr<sptk::DatabaseConnectionPool>& from,
                               const std::shared_ptr<sptk::DatabaseConnectionPool>& into,
                               int64_t& copied, std::string& problem)
    {
        copied = 0;
        problem.clear();
        if (from == nullptr)
        {
            return true; // nothing was open before this: a first start has no source to copy from
        }

        try
        {
            struct Account
            {
                std::string m_username;
                std::string m_password;
                int         m_enabled;
                std::string m_updatedAt;
                std::string m_updatedBy;
            };
            std::vector<Account> accounts;

            {
                const sptk::AutoDatabaseConnection held(*from);
                sptk::Query read(held.connection(),
                                 "SELECT username, password, is_enabled, updated_at, updated_by "
                                 "FROM xmq_user");
                read.open();
                while (!read.eof())
                {
                    accounts.push_back({.m_username = read["username"].asString().c_str(),
                                        .m_password = read["password"].asString().c_str(),
                                        .m_enabled = read["is_enabled"].asInteger(),
                                        .m_updatedAt = read["updated_at"].asString().c_str(),
                                        .m_updatedBy = read["updated_by"].asString().c_str()});
                    read.next();
                }
                read.close();
            }

            if (accounts.empty())
            {
                return true;
            }

            // One connection and one prepared statement for the lot: a store with a few thousand
            // accounts is copied while somebody is waiting for the screen to answer.
            const sptk::AutoDatabaseConnection held(*into);
            sptk::Query write(held.connection(),
                              "INSERT INTO xmq_user (username, password, is_enabled, revision, "
                              "updated_at, updated_by) "
                              "VALUES (:username, :password, :is_enabled, 1, :updated_at, :updated_by)");
            for (const auto& account: accounts)
            {
                write.param("username") = account.m_username.c_str();
                write.param("password") = account.m_password.c_str();
                write.param("is_enabled") = account.m_enabled;
                write.param("updated_at") = account.m_updatedAt.c_str();
                write.param("updated_by") = account.m_updatedBy.c_str();
                write.exec();
                ++copied;
            }
            return true;
        }
        catch (const std::exception& exception)
        {
            problem = "Could not copy the accounts across.\n\nThe database said: " +
                      std::string(exception.what());
            return false;
        }
    }

    /// One place that says what this is now authenticating against, because an empty directory and
    /// a broken one look the same from outside and only one of them is somebody's mistake.
    void announce(const std::string& shown, const int64_t accounts, const bool created)
    {
        if (created)
        {
            log(XMQ_LOG_WARNING,
                "created the xmq_user table in " + shown +
                    ", which holds no accounts - nothing can connect until accounts are added to "
                    "it. Point this at the database the broker itself uses to authenticate the "
                    "accounts on the Users screen");
            return;
        }
        log(XMQ_LOG_INFO, "authenticating against " + std::to_string(accounts) + " account(s) in " + shown);
    }

    bool start() override
    {
        // The library that got loaded, not the header this was compiled against. A soname guards
        // only the major version, so the two can differ - and when they do, the failure is a
        // mangled name or a changed structure layout, neither of which explains itself.
        if (const std::string loaded(sptk::libraryVersion()); loaded != VERSION)
        {
            log(XMQ_LOG_ERROR, "built against SPTK " + std::string(VERSION) + " but loaded SPTK " +
                                   loaded + "; rebuild this extension against the SPTK that is installed");
            return false;
        }

        std::string problem;
        std::string shown;
        int64_t     accounts = 0;
        bool        created = false;
        auto        pool = openFor(userDatabaseUri(), setting("username"), setting("password"),
                                   problem, shown, accounts, created);
        if (pool == nullptr)
        {
            // Refusing to start beats running: an authentication extension with no database
            // abstains on every client, which looks exactly like working and is not.
            log(XMQ_LOG_ERROR, problem);
            return false;
        }

        m_unknownIsDenied.store(setting("unknown_user") != "abstain");
        {
            const std::scoped_lock lock(m_databaseLock);
            m_database = std::move(pool);
        }

        announce(shown, accounts, created);
        return true;
    }

    /**
     * @brief Takes new settings while the broker runs, or refuses them and keeps what it had.
     *
     * The new database is opened and probed before anything is swapped, so a URI that names
     * nothing, an account that cannot log in, or a database with no schema is refused while the
     * old one is still authenticating. Refusing here is what makes the broker put the previous
     * values back, in memory and in the configuration file both.
     */
    /**
     * @brief Somebody saved an account or a membership in the interface.
     *
     * Just the cache. The store is the same store, opened on the same connection, and every answer
     * this extension has given about it may now be wrong - a password that was changed, an account
     * that was removed, a group somebody was taken out of. Dropping them costs one connection's
     * worth of work per client that reconnects, and keeping them costs an entry's lifetime of
     * admitting people who have been revoked.
     */
    void accountsChanged() override
    {
        m_cache.forget();
    }

    bool reload() override
    {
        std::string problem;
        std::string shown;
        int64_t     accounts = 0;
        bool        created = false;
        auto        pool = openFor(userDatabaseUri(), setting("username"), setting("password"),
                                   problem, shown, accounts, created);
        if (pool == nullptr)
        {
            // The problem alone: the host puts "refused the new settings and keeps what it had"
            // in front of it when it reports the refusal to the interface.
            log(XMQ_LOG_ERROR, problem);
            return false;
        }

        // A store this extension has just made is empty, and an operator moving from the default
        // SQLite file to a real server means to take their accounts with them - losing them is
        // never what a changed URI was asking for. Copied here rather than at start(), because
        // this is the one moment both stores are open and both are this extension's own.
        int64_t copied = 0;
        if (created)
        {
            std::shared_ptr<sptk::DatabaseConnectionPool> previous;
            {
                const std::scoped_lock lock(m_databaseLock);
                previous = m_database;
            }

            if (std::string trouble; !importAccounts(previous, pool, copied, trouble))
            {
                // Refused rather than half-applied: the new store now holds some of the accounts
                // and the old one still holds all of them, so keeping the old one is the state
                // that can be reasoned about.
                log(XMQ_LOG_ERROR, trouble);
                return false;
            }
        }

        // Only the choice of what to answer for an unknown name can change without a database
        // behind it, and it changes with the rest or not at all.
        m_unknownIsDenied.store(setting("unknown_user") != "abstain");
        {
            const std::scoped_lock lock(m_databaseLock);
            m_database = std::move(pool);
        }
        // The answers belonged to the store that has just been replaced.
        m_cache.forget();

        if (copied > 0)
        {
            log(XMQ_LOG_INFO, "copied " + std::to_string(copied) + " account(s) into " + shown);
            announce(shown, copied, false);
            return true;
        }

        announce(shown, accounts, created);
        return true;
    }

    bool stop() override
    {
        log(XMQ_LOG_INFO, "admitted " + std::to_string(m_admitted) + ", refused " +
                              std::to_string(m_refused) + " connection(s)");
        const std::scoped_lock lock(m_databaseLock);
        m_database.reset();
        return true;
    }

    xmq_auth_decision authenticate(const xmq_auth_request& request) override
    {
        const auto username = std::string(xmq::view(request.username));

        // Whether a client with no name may connect is the broker's policy and is configured there.
        // Answering it here would put the same decision in two places.
        if (username.empty())
        {
            return XMQ_AUTH_NOT_HANDLED;
        }

        // Asked before anything is locked or queried. A hit costs one HMAC where a miss costs a SQL
        // round trip and a KDF, and this runs on the broker's connect path.
        const auto password = std::string(xmq::view(request.password));
        switch (m_cache.look(username, password))
        {
            case VerificationCache::Answer::Allowed:
                ++m_admitted;
                return XMQ_AUTH_ALLOW;
            case VerificationCache::Answer::Refused:
                ++m_refused;
                return XMQ_AUTH_DENY;
            case VerificationCache::Answer::Unknown:
                break;
        }

        // Copied under the lock and used outside it, so a reload() that swaps the pool mid-query
        // cannot pull it out from under this call: the pool stays alive as long as this reference.
        std::shared_ptr<sptk::DatabaseConnectionPool> database;
        {
            const std::scoped_lock lock(m_databaseLock);
            database = m_database;
        }
        if (database == nullptr)
        {
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }

        std::string verifier;
        bool        enabled = false;
        try
        {
            const sptk::AutoDatabaseConnection connection(*database);
            sptk::Query                        query(connection.connection(),
                                                     "SELECT password, is_enabled FROM xmq_user WHERE username = :username");
            query.param("username") = username.c_str();
            query.open();

            if (query.eof())
            {
                query.close();
                ++m_refused;
                // Not remembered. "Abstain" is not a decision this extension owns, and an unknown
                // name is the one case where an account appearing a second later must be seen.
                return m_unknownIsDenied.load() ? XMQ_AUTH_DENY : XMQ_AUTH_NOT_HANDLED;
            }

            verifier = query["password"].asString().c_str();
            enabled = query["is_enabled"].asInteger() != 0;
            query.close();
        }
        catch (const std::exception& exception)
        {
            // The database, not the password. Said every time rather than once: an outage that
            // leaves one line in a log an hour ago is an outage nobody connects to the symptom.
            log(XMQ_LOG_ERROR, std::string("user database unreachable: ") + exception.what());
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }

        // An account with no password is one whose password has never been set. Nothing may be
        // authenticated against it - least of all an empty password, which is what a client that
        // sent none arrives with.
        if (!enabled || verifier.empty() || !verify(password, verifier))
        {
            ++m_refused;
            m_cache.remember(username, password, false);
            return XMQ_AUTH_DENY;
        }

        ++m_admitted;
        m_cache.remember(username, password, true);
        return XMQ_AUTH_ALLOW;
    }

private:
    // reload() replaces the pool while authenticate() is running on the broker's threads - the
    // host does not quiesce an extension to reconfigure it. The lock is held only long enough to
    // copy the pointer, and against PBKDF2 and a SQL round trip it does not show.
    VerificationCache                             m_cache;
    mutable std::mutex                            m_databaseLock;
    std::shared_ptr<sptk::DatabaseConnectionPool> m_database;
    std::atomic<bool>                             m_unknownIsDenied {true};
    uint64_t                                      m_admitted {0};
    uint64_t                                      m_refused {0};
};

} // namespace

/// What this takes, so the interface can offer a form instead of a bare key/value box - and so a
/// wrong value is refused before it reaches here rather than at the next start.
const xmq_setting userDatabaseSettings[] {
    // No database URI here. The broker gives it, through the ABI: these are the accounts the Users
    // screen edits, and the interface writes to the broker's address. A copy of that address in
    // this extension's own settings could be pointed elsewhere, and then every account added in
    // the interface would be saved, listed, and admit nobody - with no error on either side to say
    // so. The credentials stay, because they are this extension's account in that database and not
    // its address.
    {.name = "username",
     .label = "Database user",
     .description = "The account this extension opens the database with - not an MQTT client. "
                    "Leave empty for SQLite, which has no users",
     .type = XMQ_SETTING_STRING,
     .default_value = nullptr,
     .choices = nullptr,
     .required = 0},

    {.name = "password",
     .label = "Database password",
     .description = "The password for that database user. SECRET, so the interface masks it and "
                    "the broker keeps it out of the log - including the log line that names the "
                    "database when a connection fails",
     .type = XMQ_SETTING_SECRET,
     .default_value = nullptr,
     .choices = nullptr,
     .required = 0},

    {.name = "unknown_user",
     .label = "Client name not in the database",
     .description = "Deny the connection, because this extension is the account database - or "
                    "abstain, so that another authenticator beside it, such as a directory, gets "
                    "to answer for names this database has never held",
     .type = XMQ_SETTING_CHOICE,
     .default_value = "deny",
     .choices = "deny,abstain",
     .required = 0}};

XMQ_DEFINE_EXTENSION_DESCRIBED(UserDatabase, "user-database", "1.0", XMQ_CAP_AUTHENTICATOR,
                               "Authenticates MQTT clients against the broker's SQL user database",
                               userDatabaseSettings)
