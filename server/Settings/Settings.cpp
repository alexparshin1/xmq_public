/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "Settings.h"
#include "common/RedisCredentials.h"
#include "ClusterStore.h"
#include "common/DirectoryNames.h"
#include "common/HostName.h"
#include "Cluster/NodeIdentity.h"
#include "server/SelfSignedCertificate.h"
#include "PasswordHash.h"
#include "UriSecret.h"
#include "service/CUsers.h"

#include <openssl/rand.h>
#include <ranges>
#include <set>
#include <sptk5/JWT.h>

using namespace std;
using namespace sptk;
using namespace xmq;

using enum LogSubject;

namespace {

/**
 * @brief Account cluster peers connect as.
 *
 * Not an interface account: it is what one node signs in to another with, so it needs no
 * administrative rights. Setting its password is the administrator's job; a cluster reads its
 * accounts from one shared SQL store, so its nodes agree on it by construction rather than by
 * being configured one at a time. See generatedClusterPassword() for what a standalone
 * installation starts with, and why it is not a word from the source.
 */
constexpr auto clusterUsername = "cluster";

/**
 * @brief The password this installation's cluster account starts with.
 *
 * Generated, and generated once: it is written to the users file when the accounts are first
 * created, and read back from there afterwards.
 *
 * It used to be the word "cluster". Anonymous access is off by default, which sounds like enough
 * until you notice that every installation then shipped one account whose password was in the
 * source: on a broker listening on 0.0.0.0:1883, anyone who could reach the port could connect
 * and publish. Proved on a freshly installed package from another machine.
 *
 * This is what a standalone installation starts with, and it is meant to be replaced: setting the
 * account's password is the administrator's job. It costs a clustered installation nothing, since
 * the nodes of a cluster share one SQL store of accounts rather than each keeping its own.
 *
 * 24 characters out of 62 is about 143 bits, which is far past anything that has to be guessed
 * remotely over MQTT.
 */
String generatedClusterPassword()
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static constexpr size_t passwordLength = 24;
    // 248 is the largest multiple of 62 below 256. Bytes above it are drawn again rather than
    // folded with %, which would make the first eight letters of the alphabet slightly likelier.
    static constexpr unsigned char largestUnbiasedByte = 248;

    String password;
    password.reserve(passwordLength);
    while (password.length() < passwordLength)
    {
        std::array<unsigned char, passwordLength> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        {
            throw Exception("Can't generate the cluster account password: OpenSSL has no entropy");
        }
        for (const auto byte: bytes)
        {
            if (byte < largestUnbiasedByte && password.length() < passwordLength)
            {
                password += alphabet[byte % alphabet.size()];
            }
        }
    }
    return password;
}

/**
 * @brief Build an account with a password stored the way the user manager reads them.
 *
 * The password lives inside a token rather than in the file as it stands, which is what
 * UserManager decodes when it loads the accounts.
 */
CUser makeUser(const char* username, const char* password)
{
    CUser user;
    user.m_username = username;
    user.m_password = UserManager::makeUserToken(username, password, "", "", DateTime());
    user.m_is_enabled = true;
    return user;
}

/**
 * @brief The accounts a freshly configured server has.
 *
 * Both are needed for a working server: without the administrator nobody can reach the interface,
 * and the cluster account is what this node's own cluster entry is built with at startup. Nothing
 * else is: MQTT client accounts belong to whatever was connecting to the previous configuration.
 *
 * @param administratorPassword Password for the administrator, or empty for an account with no
 *                              password yet - see administratorPasswordSet().
 */
WSArray<CUser> defaultUsers(const String& administratorPassword = "", const String& clusterPassword = "")
{
    WSArray<CUser> users("users");
    users.push_back(makeUser(Settings::administratorUsername(), administratorPassword.c_str()));
    users.push_back(makeUser(clusterUsername, (clusterPassword.empty() ? generatedClusterPassword() : clusterPassword).c_str()));
    return users;
}

/**
 * @brief A configuration to start from when there is no template to copy.
 *
 * Deliberately the same as the shipped template: MQTT on 1883, MQTT+SSL on 8883, the interface on
 * 18883, and persistence off, so that a server built from it comes up and can be configured from
 * the interface. Kept as text rather than assembled from the configuration classes, because
 * writing those out is not lossless for optional elements with enumerated types.
 */
const char* defaultConfigurationText()
{
    return R"({
  "connections": {
    "listener": [
      {
        "id": 1,
        "name": "Not Encrypted",
        "bind_ip": "0.0.0.0",
        "port": 1883,
        "threads": 4,
        "protocol": "MQTT",
        "enable": true
      },
      {
        "id": 2,
        "name": "Encrypted",
        "bind_ip": "0.0.0.0",
        "port": 8883,
        "threads": 4,
        "protocol": "MQTT+SSL",
        "enable": true
      }
    ],
    "ssl_keys": {
      "cafile": "",
      "keyfile": "${ProgramCerts}/node.key",
      "certfile": "${ProgramCerts}/node.crt",
      "verify_depth": 0
    }
  },
  "queue_limits": {
    "max_size": 16384,
    "max_inflight_messages": 1024,
    "max_bytes": 0
  },
  "server_limits": {
    "send_threads": "auto",
    "receive_threads": "auto",
    "max_topic_alias": 128
  },
  "authentication": {
    "allow_anonymous": false
  },
  "persistence": {
    "redis_uri": "redis://localhost:6379",
    "clean_start": true,
    "enabled": false,
    "max_redis_connections": 2,
    "max_queued_writes": 1000
  },
  "logging": {
    "log_to": "xmq_server.log",
    "keep_logs": 7,
    "min_log_level": "INFO",
    "log_level": {
      "connect": "DEBUG",
      "disconnect": "DEBUG",
      "subscribe": "DEBUG",
      "unsubscribe": "DEBUG",
      "publish": "ERROR",
      "ack": "ERROR",
      "server_connections": "DEBUG",
      "server_events": "DEBUG",
      "cluster_connections": "INFO",
      "cluster_events": "INFO"
    }
  },
  "web_service": {
    "listener_port": 18883,
    "encrypted": true
  },
  "bridges": [],
  "cluster": {
    "this_node": {
      "node_name": "primary",
      "host_port": "localhost:8883",
      "encrypted": true
    },
    "nodes": []
  }
}
)";
}

} // namespace

Settings::Settings()
    : m_bridgeManager(this, [this]
      {
          saveConfigurationUnlocked();
      })
      , m_listenerManager(this, [this]
      {
          saveConfigurationUnlocked();
      })
      , m_userManager([this]
      {
          updateUsers();
          saveUsersUnlocked();
          saveConfigurationUnlocked();
      })
{
    constexpr auto defaultMqttPort = 1883;
    constexpr auto defaultListenerThreads = 4;
    constexpr auto defaultQueueSizeLimit = 16384;
    constexpr auto defaultInflightMessagesLimit = 1024;
    constexpr auto defaultQueueBytesLimit = 0;

    CListener defaultListener;
    defaultListener.m_protocol = "MQTT";
    defaultListener.m_port = defaultMqttPort;
    defaultListener.m_bind_ip = "0.0.0.0";
    defaultListener.m_threads = defaultListenerThreads;
    m_connections.m_listener.push_back(defaultListener);

    m_queue_limits.m_max_size = defaultQueueSizeLimit;
    m_queue_limits.m_max_inflight_messages = defaultInflightMessagesLimit;
    m_queue_limits.m_max_bytes = defaultQueueBytesLimit;

    initialize();
}

namespace {
String replacePathDelimiter(const filesystem::path& path)
{
    return String(path.string()).replace("\\\\", "/");
}

// Defined below the loading code, beside the rest of the file-writing helpers.
String expandConfigurationText(const Buffer& config, const filesystem::path& logTo);
void   collectUnknownKeys(const WSComplexType& type, const xdoc::SNode& node, const String& path,
                          Strings& warnings);
void   validateConfigurationValues(const CServerConfiguration& configuration, Strings& warnings);
}

void Settings::loadConfigurationUnlocked(const Buffer& config)
{
    const auto logTo = filesystem::path(m_logging.m_log_to.asString().c_str());
    const auto configString = expandConfigurationText(config, logTo);

    xdoc::Document document;
    document.load(configString);

    // Noted before the document is consumed: a configuration carrying an accounts element - even
    // an empty one left by an earlier version - is rewritten without it, so the element does not
    // linger in files that have already been migrated.
    const auto authenticationNode = document.root()->findFirst("authentication");
    const auto hasUsersElement = authenticationNode && authenticationNode->findFirst("users");
    const auto clusterNode = document.root()->findFirst("cluster");
    const bool hasClusterPassword = clusterNode && clusterNode->findFirst("password");

    WSComplexType::load(document.root());

    // Values the field types cannot bound, and keys nothing reads. The first stops a start with
    // the key's name; the second is a warning, because a configuration carrying an old or
    // misspelled key still runs and refusing it would strand an installation over a comment.
    Strings warnings;
    validateConfigurationValues(*this, warnings);
    collectUnknownKeys(*this, document.root(), "", warnings);
    for (const auto& warning: warnings)
    {
        CERR(warning);
    }

    initialize();
    if (hasUsersElement || hasClusterPassword)
    {
        m_changed = true;
    }
    const auto hadUsersFile = loadUsersUnlocked();

    // Whether a database is going to answer for the accounts. Asked before anything is invented,
    // because inventing starting accounts when a database already holds real ones writes an
    // administrator with no password to disk, and only then finds out it was not needed.
    const auto accountsDatabase = userDatabaseUriUnlocked();

    if (!hadUsersFile && accountsDatabase.empty() && m_authentication.m_users.empty())
    {
        // Nothing anywhere holds an account: a fresh installation, or a users file that has been
        // lost. The interface would come up with nobody able to sign in and no way to add anyone,
        // since accounts are only reachable through it. The starting accounts are created
        // instead, the administrator among them with no password at all - see
        // administratorPasswordSet() for what that state means and how it ends.
        for (const auto& user: defaultUsers())
        {
            m_authentication.m_users.push_back(user);
        }
        m_changed = true;
    }
    captureClusterSecretUnlocked();

    // Whether loading changed the accounts themselves - which it does the first time it meets one
    // still holding its password in the old, readable form.
    auto accountsConverted = false;
    m_userManager.loadConfiguration(m_authentication, accountsConverted);
    m_changed = m_changed || accountsConverted;
    // The manager owns them now. Cleared so saveConfigurationUnlocked() writes a configuration
    // without credentials in it - they go to the users file instead.
    m_authentication.m_users.clear();

    if (accountsDatabase.empty() && (!hadUsersFile || accountsConverted))
    {
        // A configuration written before accounts moved to their own file. Write them out now,
        // and before the configuration is rewritten without them: were it the other way round,
        // an interruption in between would leave the accounts in neither file and nobody able
        // to sign in.
        //
        // And again whenever loading converted an account off the old password form: converting in
        // memory and leaving the file as it was would keep every password readable there and do
        // the same conversion at every start, which is the opposite of the point.
        saveUsersUnlocked();
    }

    // After the accounts are in memory and the file has been written back, so that whichever form
    // the file was in, what moves into the database is a verifier and never a readable password.
    openUserDatabaseUnlocked();
    if (hasClusterPassword && !accountsDatabase.empty() && !m_accountsInDatabase)
        throw Exception("Cannot migrate the legacy cluster credential while the accounts database is unavailable. "
                        "The configuration was not rewritten.");
    if (m_accountsInDatabase && m_accountsPool)
    {
        UserStore store(m_accountsPool);
        // Migrate a legacy configuration secret only if its verifier agrees. A stale
        // node must never replace the shared credential or change the cluster account.
        const auto account = store.findUser("cluster");
        const auto legacy = m_cluster.m_password.asString();
        if (store.clusterSecret().empty() && account && account->m_enabled &&
            !legacy.empty() && PasswordHash::verify(legacy, account->m_password))
            store.setClusterSecret(legacy);
        m_cluster.m_password = "";
    }

    // After the accounts: the cluster's settings come from the same database, through the same
    // pool, and one that cannot be read stops the start rather than being guessed at.
    applyClusterConfigurationUnlocked();

    m_bridgeManager.initialize(m_changed);
    m_listenerManager.initialize(m_changed);

    if (m_changed)
    {
        saveConfigurationUnlocked();
        m_changed = false;
    }
}

void Settings::loadConfiguration(const Buffer& config)
{
    const WriteLock lock(m_mutex);

    loadConfigurationUnlocked(config);
}

void Settings::loadConfiguration(const filesystem::path& configurationFile, const filesystem::path& usersFile)
{
    const WriteLock lock(m_mutex);

    m_usersPath = usersFile;

    if (!filesystem::exists(configurationFile))
    {
        throw invalid_argument("The configuration file doesn't exist: " + configurationFile.string());
    }

    Buffer config;
    config.loadFromFile(configurationFile);
    m_configurationPath = configurationFile;
    loadConfigurationUnlocked(config);
}

void Settings::loadConfigurationFrom(const filesystem::path& configurationFile,
                                     const filesystem::path& lastGoodFile,
                                     const filesystem::path& usersFile)
{
    const WriteLock lock(m_mutex);

    m_usersPath = usersFile;
    m_configurationPath = configurationFile;

    Buffer config;
    config.loadFromFile(lastGoodFile);
    m_startupWarning = "Configuration file " + configurationFile.string() +
                       " could not be read; the last good configuration was used instead (" +
                       lastGoodFile.string() + ").";
    loadConfigurationUnlocked(config);
}

String Settings::startupWarning() const
{
    const ReadLock lock(m_mutex);
    return m_startupWarning;
}

void Settings::updateUsers()
{
    const WriteLock lock(m_mutex);

    // Only allow_anonymous belongs in the configuration file; the accounts go to the users file.
    const auto allowAnonymous = m_userManager.isAllowAnonymous();
    m_authentication.m_allow_anonymous = allowAnonymous;
}

void Settings::saveConfigurationUnlocked(Buffer& config) const
{
    // The write is guarded as well as the read: an edit through the interface must not be able to
    // put a file on disk that the next start will refuse. Warnings are not repeated here - they
    // belong to the load, where the key came from the file rather than from a field being set.
    Strings warnings;
    validateConfigurationValues(*this, warnings);

    xdoc::Document document;
    WSComplexType::unload(document.root());
    if (const auto cluster = document.root()->findFirst("cluster")) cluster->remove("password");

    // Accounts live in the users file, so the configuration should not carry the element at all -
    // not even as an empty list. Leaving it there invites someone to add an account to the wrong
    // file. The type still has it so a configuration written before the split still loads.
    if (const auto authentication = document.root()->findFirst("authentication");
        authentication)
    {
        authentication->remove("users");
    }

    document.exportTo(xdoc::DataFormat::JSON, config, true);
}

namespace {

/**
 * @brief Keeps a file readable by its owner and its group, and by nobody else.
 *
 * These files carry secrets a broker has to be able to present: the password in a database URI,
 * the cluster secret, a bridge's credentials. They were installed and written at 0644, so every
 * account on the machine could read them.
 *
 * Applied after writing rather than by setting a umask, because the umask is process-wide and a
 * broker that quietly narrows every file it touches is a surprise waiting for somebody. Failures
 * are reported and not thrown: a configuration that was written is worth more than one refused
 * over a permission that some filesystems do not have.
 */
void restrictToOwnerAndGroup(const filesystem::path& path)
{
    error_code errorCode;
    filesystem::permissions(path,
                            filesystem::perms::owner_read | filesystem::perms::owner_write |
                            filesystem::perms::group_read,
                            filesystem::perm_options::replace, errorCode);
    if (errorCode)
    {
        CERR("Cannot restrict the permissions of " << path.string() << ": " << errorCode.message());
    }
}

/**
 * @brief Write a file so that a reader never sees a half-written one.
 *
 * Written beside the target and renamed over it: rename is atomic on the filesystems an
 * installation lives on, so the file is either the old one or the new one. A crash in the middle
 * of a write used to leave a truncated configuration, which is exactly what a broker cannot start
 * from and what an interface cannot repair.
 */
void saveAtomically(const filesystem::path& path, const Buffer& data)
{
    auto temporary = path;
    temporary += ".tmp";

    try
    {
        data.saveToFile(temporary);
        restrictToOwnerAndGroup(temporary);

        error_code errorCode;
        filesystem::rename(temporary, path, errorCode);
        if (errorCode)
        {
            throw Exception("Can't replace " + path.string() + ": " + errorCode.message());
        }
    }
    catch (...)
    {
        error_code ignored;
        filesystem::remove(temporary, ignored);
        throw;
    }
}

/**
 * @brief Resolve the placeholders a configuration may carry.
 *
 * Kept apart from loading so that --check-config reads a file the same way a start does without
 * starting anything.
 */
String expandConfigurationText(const Buffer& config, const filesystem::path& logTo)
{
    String configString(config);
    return configString.replace(R"(\$\{ProgramData\})", replacePathDelimiter(DirectoryNames::confDirectory()))
        .replace(R"(\$\{ProgramCerts\})", replacePathDelimiter(DirectoryNames::certsDirectory()))
        .replace(R"(\$\{ProgramTemp\})", replacePathDelimiter(DirectoryNames::tempDirectory()))
        .replace(R"(\$\{ProgramLogs\})", replacePathDelimiter(logTo));
}

/**
 * @brief Keys an older configuration may carry that are deliberately no longer read.
 *
 * They are handled by the migration in loadConfigurationUnlocked() rather than ignored: the
 * accounts once lived in the configuration, and the cluster secret used to as well. Warning about
 * them on every start would train the reader to ignore the warning that matters.
 */
bool isLegacyConfigurationKey(const String& path, const String& name)
{
    return (path == "authentication" && name == "users") ||
           (path == "cluster" && name == "password") ||
           (path == "persistence" && name == "max_threads") ||
           (path == "logging" && name == "log_level_available");
}

/**
 * @brief Warn about object keys the configuration does not define.
 *
 * A mistyped key is accepted by the loader, which reads the fields it knows and leaves the rest
 * alone, so the setting silently does nothing. Walking the document against the same field lists
 * the loader uses turns that into something the reader can see. Arrays are not descended into:
 * their elements are the generated types' business, and the keys inside them have no effect on
 * the file as a whole.
 */
void collectUnknownKeys(const WSComplexType& type, const xdoc::SNode& node, const String& path,
                        Strings& warnings)
{
    for (const auto& child: node->nodes())
    {
        const String name(std::string(child->getName()));
        String       childPath = path;
        if (!childPath.empty())
        {
            childPath += ".";
        }
        childPath += name;

        WSType* field = nullptr;
        try
        {
            field = type.getField(name);
        }
        catch (const Exception&)
        {
            if (!isLegacyConfigurationKey(path, name))
            {
                warnings.push_back("Unknown configuration key '" + childPath +
                                   "' has no effect. Check its spelling.");
            }
            continue;
        }

        if (const auto* complex = dynamic_cast<const WSComplexType*>(field))
        {
            collectUnknownKeys(*complex, child, childPath, warnings);
        }
    }
}

/**
 * @brief Check the values whose range the field types cannot express.
 *
 * Throws on the first value that would stop the broker from serving, naming the key rather than
 * the class that carries it. Returns what is unusual but usable in @p warnings.
 */
void validateConfigurationValues(const CServerConfiguration& configuration, Strings& warnings)
{
    const auto checkThreadCount = [](const String& value, const String& key)
    {
        const auto text = String(value).trim().toLowerCase();
        if (text.empty() || text == "auto" || text == "0")
        {
            return;
        }
        const auto count = text.toInt();
        if (count < 1 || count > 256)
        {
            throw invalid_argument("The configuration key '" + key +
                                   "' should be \"auto\", 0, or a number in range [1..256].");
        }
    };

    checkThreadCount(configuration.m_server_limits.m_send_threads.asString(),
                     "server_limits.send_threads");
    checkThreadCount(configuration.m_server_limits.m_receive_threads.asString(),
                     "server_limits.receive_threads");
    checkThreadCount(configuration.m_server_limits.m_delivery_threads.asString(),
                     "server_limits.delivery_threads");

    const auto checkPort = [](const WSInteger& value, const String& key, const bool allowZero)
    {
        if (value.isNull())
        {
            return;
        }
        const auto port = value.asInteger();
        if (port < (allowZero ? 0 : 1) || port > 65535)
        {
            throw invalid_argument("The configuration key '" + key + "' should be a port number" +
                                   (allowZero ? " between 0 and 65535." : " between 1 and 65535."));
        }
    };

    size_t listenerIndex = 0;
    for (const auto& listener: configuration.m_connections.m_listener)
    {
        const auto prefix = "connections.listener[" + to_string(listenerIndex++) + "]";
        checkPort(listener.m_port, prefix + ".port", false);
        if (!listener.m_threads.isNull())
        {
            const auto threads = listener.m_threads.asInteger();
            if (threads < 1 || threads > 256)
            {
                throw invalid_argument("The configuration key '" + prefix +
                                       ".threads' should be a number in range [1..256].");
            }
        }
        if (listener.m_protocol.asString().empty())
        {
            throw invalid_argument("The configuration key '" + prefix + ".protocol' cannot be empty.");
        }
    }

    checkPort(configuration.m_web_service.m_listener_port, "web_service.listener_port", true);

    // Zero is not a bad lease: Cluster.cpp reads it, like an absent setting, as the default.
    if (!configuration.m_cluster.m_lease_seconds.isNull() &&
        configuration.m_cluster.m_lease_seconds.asInteger() < 0)
    {
        throw invalid_argument("The configuration key 'cluster.lease_seconds' should not be negative.");
    }
    if (!configuration.m_cluster.m_links.isNull() && configuration.m_cluster.m_links.asInteger() < 0)
    {
        warnings.push_back("The configuration key 'cluster.links' is negative; a node will open no links.");
    }
}

} // namespace

void Settings::saveConfigurationUnlocked() const
{
    if (m_configurationPath.empty())
    {
        return;
    }

    Buffer config;
    saveConfigurationUnlocked(config);
    saveAtomically(m_configurationPath, config);
}

filesystem::path Settings::usersPathUnlocked() const
{
    if (!m_usersPath.empty())
    {
        return m_usersPath;
    }
    if (m_configurationPath.empty())
    {
        return {};
    }
    // Beside the configuration, so two servers whose configurations live in separate directories
    // keep separate accounts without either being told where to look.
    return filesystem::path(m_configurationPath).replace_filename("xmq_users.conf");
}

String Settings::userDatabaseUri() const
{
    const ReadLock lock(m_mutex);

    return userDatabaseUriUnlocked();
}

String Settings::userDatabaseUriUnlocked() const
{
    if (const auto configured = m_authentication.m_database_uri.asString();
        !configured.empty())
    {
        return configured;
    }

    const auto usersPath = usersPathUnlocked();
    if (usersPath.empty())
    {
        return {};
    }

    return DirectoryNames::sqliteUri(filesystem::path(usersPath).replace_filename("xmq_users.db"));
}

void Settings::setAccountDatabaseUri(const std::string& uri)
{
    const WriteLock lock(m_mutex);

    if (m_authentication.m_database_uri.asString() == sptk::String(uri))
    {
        return;
    }

    m_authentication.m_database_uri = uri;
    saveConfigurationUnlocked();
}

int Settings::passwordIterations() const
{
    const ReadLock lock(m_mutex);

    return passwordIterationsUnlocked();
}

int Settings::passwordIterationsUnlocked() const
{
    if (m_authentication.m_password_iterations.isNull())
    {
        return PasswordHash::defaultIterations;
    }
    return m_authentication.m_password_iterations.asInteger();
}

void Settings::captureClusterSecretUnlocked()
{
    if (!m_cluster.m_password.asString().empty())
    {
        return;
    }

    for (const auto& user: m_authentication.m_users)
    {
        if (String(user.m_username.asString()) != clusterUsername)
        {
            continue;
        }
        if (const auto secret = UserManager::legacyPassword(String(user.m_password.asString()));
            secret && !secret->empty())
        {
            m_cluster.m_password = *secret;
            m_changed = true;
        }
        return;
    }
}

void Settings::openUserDatabaseUnlocked()
{
    const auto uri = userDatabaseUriUnlocked();
    if (uri.empty())
    {
        // A configuration that came from no file, which is how the tests build one: there is
        // nowhere to put a default database and nothing that asked for a particular one.
        return;
    }

    try
    {
        constexpr size_t connections = 4;
        m_accountsPool = std::make_shared<DatabaseConnectionPool>(uri, static_cast<unsigned>(connections));
        auto store = std::make_shared<UserStore>(m_accountsPool);
        store->createSchema();
        const auto usersPath = usersPathUnlocked();
        if (const auto migrated = store->importUsersFrom(usersPath, passwordIterationsUnlocked());
            migrated > 0)
        {
            COUT("Migrated " << migrated << " account(s) into " << UriSecret::hidden(uri) << endl);
        }
        m_userManager.useStore(store);
        m_accountsInDatabase = true;

        // Whichever way the accounts got here, the two groups every installation has are here
        // too. Being an administrator is a membership of one of them, so a database without them
        // is a broker nobody can administer.
        const auto [defaultGroup, administratorsGroup] = store->createStandardGroups();

        if (store->users().empty())
        {
            // A database nobody has used yet, and no file to migrate: the starting accounts go
            // straight into it. Written here rather than earlier so that a database which does
            // hold accounts is never overwritten by invented ones.
            m_authentication.m_users = defaultUsers();
            captureClusterSecretUnlocked();

            auto converted = false;
            m_userManager.loadConfiguration(m_authentication, converted);
            m_authentication.m_users.clear();
            m_userManager.replaceStoredAccounts();

            for (const auto& account: store->users())
            {
                store->addUserToGroup(account.m_id, defaultGroup);
                if (account.m_username == administratorUsername())
                {
                    store->addUserToGroup(account.m_id, administratorsGroup);
                }
            }
        }

        // Put aside rather than removed. It is the only other copy of the accounts, and a
        // migration that went wrong is noticed afterwards - but it must not be left where it is,
        // or the next start reads it as a source again and the two disagree.
        if (!usersPath.empty())
        {
            error_code errorCode;
            if (filesystem::exists(usersPath, errorCode))
            {
                // Written back before it is put aside, so that the copy kept for safety holds
                // verifiers and not the passwords the old form left readable in it. A recovery
                // copy is worth keeping; a readable password in it is the thing this change is about.
                saveUsersToFileUnlocked(usersPath);

                const auto asideName = filesystem::path(usersPath).replace_extension(".hide");
                filesystem::rename(usersPath, asideName, errorCode);
                if (errorCode)
                {
                    CERR("Cannot move " << usersPath.string() << " aside: " << errorCode.message()
                        << ". The accounts are in the database; this file is no longer read.");
                }
                else
                {
                    COUT("The accounts now live in the database; " << usersPath.filename().string()
                        << " was kept as " << asideName.filename().string() << endl);
                }
            }
        }
    }
    catch (const Exception& e)
    {
        // Said with the address so it can be fixed, and with the password taken out of it so the
        // log does not become the place the secret ends up.
        m_accountsError = e.what();
        CERR("Cannot open the accounts database " << UriSecret::hidden(uri) << ": " << e.what()
            << ". The accounts read from the file are used instead.");
    }
}

void Settings::applyClusterConfigurationUnlocked()
{
    const auto uri = userDatabaseUriUnlocked();
    if (uri.empty())
    {
        // No database at all: nothing is shared, and the file is the whole configuration.
        return;
    }

    // Asked of the URI rather than of a connection: an unreachable database is exactly the case
    // this decides about, and reaching out to learn the driver would fail with it.
    const auto scheme = String(uri).toLowerCase().substr(0, String(uri).find(':'));
    const auto isClusterDatabase = scheme.starts_with("postgres") || scheme.starts_with("mysql") || scheme.starts_with("mariadb");
    if (!isClusterDatabase)
    {
        // SQLite is one machine's file; a cluster's configuration cannot live in one.
        return;
    }

    m_clusterConfigurationError = "";
    if (!m_accountsPool)
    {
        // Recorded, not thrown: loading a configuration is also what tools and tests do without
        // starting anything, and failing there would take them down for a database they never
        // wanted. The server refuses to start with this, before it serves a single client - a node
        // that cannot read whether it belongs to a cluster has no safe guess.
        stringstream message;
        message << "The cluster configuration is kept in the database " << UriSecret::hidden(uri)
            << ", and it cannot be opened: " << m_accountsError
            << ". A node that cannot read whether it belongs to a cluster does not start.";
        m_clusterConfigurationError = message.str();
        return;
    }

    try
    {
        ClusterStore store(m_accountsPool);
        store.createSchema();

        // Read from the member, not through configurationPath(): this runs under the configuration's
        // write lock, and that accessor takes the read lock of the same mutex - a lock this thread
        // already holds, taken again as a reader, which waits for the writer to finish and never does.
        const auto configuration = m_configurationPath;
        if (configuration.empty())
        {
            // Built from a buffer, as the tests build one: nowhere to keep a GUID, so nothing to key
            // the row on. A node the broker runs from always has a configuration path.
            return;
        }

        const auto nodeId = cluster::NodeIdentity::load(filesystem::path(configuration).replace_filename(cluster::NodeIdentity::FileName));

        if (const auto row = store.node(nodeId))
        {
            if (row->m_state == "Removed" || row->m_state == "Retired")
            {
                throw Exception("This node's configuration in this database was retired or removed. "
                                "Select a different database or explicitly restore its configuration; "
                                "the old configuration will not be recreated automatically.");
            }
            // The database is the configuration from here on. The accounts URI is not taken from the
            // row: it is the address this node reached the database by, and a different one there
            // says nothing about where this node is.
            m_persistence.m_redis_uri = row->m_redisUri;
            m_cluster.m_database_uri = row->m_databaseUri;
            // Invited is an unaccepted offer, including rows written by older versions.
            m_cluster.m_enabled = row->m_clusterEnabled && row->m_state != "Invited";
            m_cluster.m_lease_seconds = row->m_leaseSeconds;
            m_cluster.m_links = row->m_links;
            m_cluster.m_this_node.m_node_name = row->m_name;
            m_cluster.m_this_node.m_host_port = row->m_hostPort;

            // Write this node's certificate to the shared database so other nodes find it
            // there. Kept up to date on every start, so a reissued certificate is picked up.
            const auto configuredCertificate = m_connections.m_ssl_keys.m_certfile.asString();
            const auto certFile = configuredCertificate.empty() ? nodeKeyFiles().first : filesystem::path(configuredCertificate.c_str());
            if (error_code ec; filesystem::exists(certFile, ec))
            {
                Buffer pem;
                pem.loadFromFile(certFile);
                if (const String pemContent(pem.c_str(), pem.size());
                    row->m_certificate != pemContent)
                {
                    auto updated = *row;
                    updated.m_certificate = pemContent;
                    store.updateNode(updated);
                }
            }

            // Synchronise certificates: every other node's PEM becomes a trusted peer file.
            syncPeerCertificates(store, nodeId);
            return;
        }

        // First start on this database: the row is created from the configuration the node came with,
        // so that the interface has something to edit and the next start has a row to read.
        // But a declaration with the same name may already exist — placed by another node that
        // expects this one to join (ControlService::addClusterNode).  If the name is taken, re-key
        // the existing row with the real GUID instead of inserting a duplicate.
        if (const auto existing = store.nodeByName(m_cluster.m_this_node.m_node_name.asString()))
        {
            auto updated = *existing;
            updated.m_nodeId = nodeId;
            updated.m_hostPort = m_cluster.m_this_node.m_host_port.asString();
            updated.m_redisUri = m_persistence.m_redis_uri.asString();
            updated.m_databaseUri = uri;
            updated.m_clusterEnabled = !m_cluster.m_enabled.isNull() && m_cluster.m_enabled.asBool();
            updated.m_leaseSeconds = m_cluster.m_lease_seconds.isNull() ? 0 : static_cast<int>(m_cluster.m_lease_seconds.asInteger());
            updated.m_links = m_cluster.m_links.isNull() ? 0 : static_cast<int>(m_cluster.m_links.asInteger());
            {
                const auto configuredCertificate = m_connections.m_ssl_keys.m_certfile.asString();
                const auto certFile = configuredCertificate.empty() ? nodeKeyFiles().first : filesystem::path(configuredCertificate.c_str());
                if (error_code ec; filesystem::exists(certFile, ec))
                {
                    Buffer pem;
                    pem.loadFromFile(certFile);
                    updated.m_certificate = String(pem.c_str(), pem.size());
                }
            }
            store.updateNode(updated);

            // Populate in-memory state from the re-keyed row.
            m_persistence.m_redis_uri = updated.m_redisUri;
            m_cluster.m_database_uri = updated.m_databaseUri;
            m_cluster.m_enabled = updated.m_clusterEnabled && updated.m_state != "Invited";
            m_cluster.m_lease_seconds = updated.m_leaseSeconds;
            m_cluster.m_links = updated.m_links;
            m_cluster.m_this_node.m_node_name = updated.m_name;
            m_cluster.m_this_node.m_host_port = updated.m_hostPort;

            syncPeerCertificates(store, nodeId);
            return;
        }

        ClusterStore::Node row;
        row.m_nodeId = nodeId;
        row.m_name = m_cluster.m_this_node.m_node_name.asString();
        row.m_hostPort = m_cluster.m_this_node.m_host_port.asString();
        row.m_state = "Standalone";
        row.m_redisUri = m_persistence.m_redis_uri.asString();
        row.m_databaseUri = uri;
        row.m_clusterEnabled = !m_cluster.m_enabled.isNull() && m_cluster.m_enabled.asBool();
        row.m_leaseSeconds = m_cluster.m_lease_seconds.isNull() ? 0 : static_cast<int>(m_cluster.m_lease_seconds.asInteger());
        row.m_links = m_cluster.m_links.isNull() ? 0 : static_cast<int>(m_cluster.m_links.asInteger());
        {
            const auto configuredCertificate = m_connections.m_ssl_keys.m_certfile.asString();
            const auto certFile = configuredCertificate.empty() ? nodeKeyFiles().first : filesystem::path(configuredCertificate.c_str());
            if (error_code ec; filesystem::exists(certFile, ec))
            {
                Buffer pem;
                pem.loadFromFile(certFile);
                row.m_certificate = String(pem.c_str(), pem.size());
            }
        }
        store.insertNode(row);

        // Synchronise certificates: every other node's PEM becomes a trusted peer file.
        syncPeerCertificates(store, nodeId);
    }
    catch (const Exception& e)
    {
        // Recorded, as an unreachable database is: the configuration loads - tools and tests read
        // one without starting a broker - and the server refuses to start with this reason.
        stringstream message;
        message << "The cluster configuration is kept in the database " << UriSecret::hidden(uri)
            << ", and it cannot be read: " << e.what()
            << ". A node that cannot read whether it belongs to a cluster does not start.";
        m_clusterConfigurationError = message.str();
    }
}


void Settings::syncPeerCertificates(ClusterStore& store, const String& nodeId)
{
    const auto peersDir = peerCertificatesDirectory();
    error_code ec;
    if (!filesystem::exists(peersDir, ec))
    {
        filesystem::create_directories(peersDir, ec);
    }

    size_t written = 0;
    for (const auto& other: store.nodes())
    {
        if (other.m_nodeId == nodeId || other.m_certificate.empty())
        {
            continue;
        }
        const auto peerFile = peersDir / (other.m_name.c_str() + String(".crt"));
        if (filesystem::exists(peerFile, ec))
        {
            Buffer existing;
            existing.loadFromFile(peerFile);
            if (String(existing.c_str(), existing.size()) == other.m_certificate)
            {
                continue;
            }
        }
        Buffer certData(other.m_certificate.c_str(), other.m_certificate.size());
        certData.saveToFile(peerFile);
        ++written;
    }

    if (written > 0)
    {
        (void) buildPeerCertificateBundle();
    }
}

void Settings::refreshPeerCertificates()
{
    const WriteLock lock(m_mutex);
    if (!m_accountsPool || m_configurationPath.empty()) return;
    ClusterStore store(m_accountsPool);
    if (!store.isClusterDatabase()) return;
    const auto uuid = cluster::NodeIdentity::load(
        filesystem::path(m_configurationPath).replace_filename(cluster::NodeIdentity::FileName));
    syncPeerCertificates(store, uuid);
}

String Settings::applyInvitationAccept(const String& redisUri, const String& databaseUri)
{
    // Admission is written only by Accept, never by the inviter. The next load reads this row
    // instead of the bootstrap file, so both must carry the accepted settings.
    try
    {
        auto pool = make_shared<DatabaseConnectionPool>(databaseUri, 1);
        ClusterStore store(pool);
        store.createSchema();
        auto row = store.node(nodeIdentity());
        if (!row || row->m_state == "Removed" || row->m_state == "Retired")
        {
            return "The invitation has no usable node configuration in the target database.";
        }
        row->m_redisUri = redisUri;
        row->m_databaseUri = databaseUri;
        row->m_clusterEnabled = true;
        row->m_state = "Joining";
        store.updateNode(*row);
        store.clearInvitation(row->m_nodeId);
    }
    catch (const Exception& e)
    {
        return e.what();
    }
    {
        const WriteLock lock(m_mutex);

        // The previous configuration is the last-good one: kept beside the new one, so a start
        // that cannot join the cluster has what it was serving to go back to.
        if (!m_configurationPath.empty() && filesystem::exists(m_configurationPath))
        {
            error_code errorCode;
            filesystem::copy_file(m_configurationPath, lastGoodPath(), filesystem::copy_options::overwrite_existing, errorCode);
            if (errorCode)
            {
                return String("Cannot keep the previous configuration: ") + errorCode.message();
            }
        }

        m_persistence.m_redis_uri = redisUri;
        m_persistence.m_enabled = true;
        m_authentication.m_database_uri = databaseUri;
        m_cluster.m_enabled = true;

        // The bootstrap file is where the SQL URI lives; the next start reads the rest from the
        // shared database, by this node's GUID. Written now, so a crash between here and the
        // restart still leaves the accepted configuration on disk.
        try
        {
            saveConfigurationUnlocked();
        }
        catch (const Exception& e)
        {
            return e.what();
        }
    }
    return "";
}

filesystem::path Settings::lastGoodPath() const
{
    return lastGoodConfigurationPath(m_configurationPath);
}

String Settings::keepLastGoodConfiguration()
{
    const WriteLock lock(m_mutex);
    if (m_configurationPath.empty()) return {};
    try
    {
        Buffer current;
        saveConfigurationUnlocked(current);
        saveAtomically(lastGoodPath(), current);
        return {};
    }
    catch (const exception& error)
    {
        return error.what();
    }
}

String Settings::rollbackPendingAccept(const String& reason)
{
    {
        const WriteLock lock(m_mutex);

        if (!filesystem::exists(lastGoodPath()))
        {
            return "There is no pending accept to roll back.";
        }

        Buffer previous;
        previous.loadFromFile(lastGoodPath());
        xdoc::Document document;
        document.load(String(previous));
        CServerConfiguration previousSettings;
        previousSettings.load(document.root());
        const auto wasClustered = !previousSettings.m_cluster.m_enabled.isNull() &&
                                  previousSettings.m_cluster.m_enabled.asBool();

        // Restore SQL as well as the bootstrap: SQL overrides the file when it is loaded.
        // A failed join remains visible as Failed; a failed leave returns to cluster membership.
        // As best it can be reached:
        // the very failure being handled may be that the database is not.
        if (m_accountsPool)
        {
            try
            {
                ClusterStore store(m_accountsPool);
                store.createSchema();
                const auto nodeId = cluster::NodeIdentity::load(filesystem::path(m_configurationPath).replace_filename(cluster::NodeIdentity::FileName));
                if (const auto row = store.node(nodeId))
                {
                    auto failed = *row;
                    failed.m_state = wasClustered ? "Joining" : "Failed";
                    failed.m_clusterEnabled = wasClustered;
                    failed.m_redisUri = previousSettings.m_persistence.m_redis_uri.asString();
                    failed.m_lastError = reason;
                    store.updateNode(failed);
                }
            }
            catch (const Exception&)
            {
                // The rollback itself must go through; the Failed row is best-effort.
            }
        }

        // The previous configuration, as it was: the last-good copy is written over the failed
        // one, and the marker goes with it. The controller reloads it before recreating MQTT.
        saveAtomically(m_configurationPath, previous);

        error_code errorCode;
        filesystem::remove(lastGoodPath(), errorCode);
    }
    return "";
}

shared_ptr<DatabaseConnectionPool> Settings::accountsPool() const
{
    const ReadLock lock(m_mutex);
    return m_accountsPool;
}

String Settings::nodeIdentity() const
{
    const ReadLock lock(m_mutex);

    if (m_configurationPath.empty())
    {
        return {};
    }

    return cluster::NodeIdentity::load(filesystem::path(m_configurationPath).replace_filename(cluster::NodeIdentity::FileName));
}

shared_ptr<ClusterStore> Settings::clusterStore() const
{
    const ReadLock lock(m_mutex);

    if (!m_accountsPool)
    {
        return {};
    }

    return make_shared<ClusterStore>(m_accountsPool);
}

String Settings::applyClusterEnable(const String& redisUri, const String& databaseUri)
{
    {
        const WriteLock lock(m_mutex);

        // The way back, as after an accept: a start that cannot join - the storage holds another
        // cluster, the Redis is gone - puts the previous configuration back and says why.
        if (!m_configurationPath.empty() && filesystem::exists(m_configurationPath))
        {
            error_code errorCode;
            filesystem::copy_file(m_configurationPath, lastGoodPath(), filesystem::copy_options::overwrite_existing, errorCode);
            if (errorCode)
            {
                return String("Cannot keep the previous configuration: ") + errorCode.message();
            }
        }

        m_persistence.m_redis_uri = redisUri;
        m_persistence.m_enabled = true;
        m_authentication.m_database_uri = databaseUri;
        m_cluster.m_enabled = true;

        try
        {
            saveConfigurationUnlocked();
        }
        catch (const Exception& e)
        {
            return e.what();
        }
    }

    // The row says the same thing, so that the next start and the screen read it from the shared
    // database like every other cluster setting.
    if (const auto pool = accountsPool(); pool)
    {
        try
        {
            ClusterStore store(pool);
            store.createSchema();
            if (const auto row = store.node(nodeIdentity()))
            {
                auto enabled = *row;
                enabled.m_redisUri = redisUri;
                enabled.m_clusterEnabled = true;
                store.updateNode(enabled);
            }
        }
        catch (const Exception& e)
        {
            return String("The node's row was not updated: ") + e.what();
        }
    }

    return "";
}

String Settings::applyClusterDisable()
{
    {
        const WriteLock lock(m_mutex);

        m_cluster.m_enabled = false;

        try
        {
            saveConfigurationUnlocked();
        }
        catch (const Exception& e)
        {
            return e.what();
        }
    }

    // The row as well as the file: the storage stays as it is, because a node that leaves keeps
    // the storage it knows - only its membership goes.
    if (const auto pool = accountsPool(); pool)
    {
        try
        {
            ClusterStore store(pool);
            store.createSchema();
            if (const auto row = store.node(nodeIdentity()))
            {
                auto disabled = *row;
                disabled.m_clusterEnabled = false;
                disabled.m_state = "Standalone";
                disabled.m_lastError = "";
                store.updateNode(disabled);
            }
        }
        catch (const Exception& e)
        {
            return String("The node's row was not updated: ") + e.what();
        }
    }

    return "";
}

String Settings::clusterConfigurationError() const
{
    const ReadLock lock(m_mutex);
    return m_clusterConfigurationError;
}

String Settings::clusterPassword() const
{
    const ReadLock lock(m_mutex);
    if (m_accountsPool) return UserStore(m_accountsPool).clusterSecret();
    return m_cluster.m_password.asString();
}

bool Settings::loadUsersUnlocked()
{
    const auto usersPath = usersPathUnlocked();
    if (usersPath.empty())
    {
        return true;
    }

    error_code errorCode;
    if (!filesystem::exists(usersPath, errorCode))
    {
        // No users file yet: the accounts the configuration carries are used, and the caller
        // writes them out to the users file.
        m_changed = true;
        return false;
    }

    Buffer usersConfig;
    usersConfig.loadFromFile(usersPath);

    xdoc::Document document;
    document.load(String(usersConfig));

    CUsers users;
    users.load(document.root());

    if (!m_authentication.m_users.empty())
    {
        // Both files carry accounts. The users file is the one that counts, and saying so beats
        // letting someone add an account to the configuration and wonder why it never works.
        CERR("Accounts found in " << m_configurationPath.string() << " are ignored: "
            << usersPath.string() << " is where they are read from.");
    }

    m_authentication.m_users = users.m_users;
    return true;
}

void Settings::saveUsersUnlocked() const
{
    if (m_accountsInDatabase)
    {
        // The database is where the accounts live now, and the manager has already written them
        // there. Writing the file as well would leave two answers to the same question, and which
        // of them a restart believed would come down to the order two lines happen to be in.
        return;
    }

    const auto usersPath = usersPathUnlocked();
    if (usersPath.empty())
    {
        return;
    }

    saveUsersToFileUnlocked(usersPath);
}

void Settings::saveUsersToFileUnlocked(const filesystem::path& usersPath) const
{

    CUsers users;
    users.m_users = m_userManager.getUsers(".*", false);

    xdoc::Document document;
    users.unload(document.root());

    Buffer usersConfig;
    document.exportTo(xdoc::DataFormat::JSON, usersConfig, true);
    saveAtomically(usersPath, usersConfig);
}

void Settings::initialize()
{
    m_logSubjectPriorities.initialize(m_logging);

    if (m_server_limits.m_max_packet_size.asInteger() < 1)
    {
        m_server_limits.m_max_packet_size = 256 * 1024 * 1024;
    }

    // Encrypted unless the configuration says otherwise, including when it says nothing: a
    // configuration written before the interface had TLS should come back up with it, not without.
    if (m_web_service.m_encrypted.isNull())
    {
        m_web_service.m_encrypted = true;
    }
    // Filled in rather than left to the code that reads them, so the paths appear in the saved
    // configuration and can be pointed elsewhere without anyone having to know the default.
    // Both sections default to the same pair - the one issued for this node - because a pair
    // issued here is this server's identity, and it has only one. Pointing them at different
    // files afterwards is supported and is what installing a certificate for one of them does.
    const auto [nodeCertificate, nodeKey] = nodeKeyFiles();

    if (m_web_service.m_certfile.asString().empty())
    {
        m_web_service.m_certfile = nodeCertificate.string();
    }
    if (m_web_service.m_keyfile.asString().empty())
    {
        m_web_service.m_keyfile = nodeKey.string();
    }

    // Left alone when the configuration names something, which is how an installation that
    // already had broker keys keeps them.
    if (m_connections.m_ssl_keys.m_certfile.asString().empty())
    {
        m_connections.m_ssl_keys.m_certfile = nodeCertificate.string();
    }
    if (m_connections.m_ssl_keys.m_keyfile.asString().empty())
    {
        m_connections.m_ssl_keys.m_keyfile = nodeKey.string();
    }
}

LogPriority Settings::getLogPriority() const
{
    const ReadLock lock(m_mutex);

    return m_logSubjectPriorities.getLogPriority();
}

void Settings::setLogPriority(const LogPriority logPriority)
{
    m_logSubjectPriorities.setLogPriority(logPriority);
}

void Settings::setLogSubjectPriority(const LogSubject messageSubject, const LogPriority messagePriority)
{
    m_logSubjectPriorities.setLogSubjectPriority(messageSubject, messagePriority);
}

void Settings::setLogSubjectsPriority(const std::initializer_list<LogSubject> messageSubjects, const LogPriority priority)
{
    m_logSubjectPriorities.setLogSubjectsPriority(messageSubjects, priority);
}

void Settings::setLogSubjectsPriority(const vector<LogSubject>& messageSubjects, const LogPriority messagePriority)
{
    m_logSubjectPriorities.setLogSubjectsPriority(messageSubjects, messagePriority);
}

bool Settings::logSubjectEnabled(const LogSubject messageSubject, const LogPriority messagePriority) const
{
    return m_logSubjectPriorities.logSubjectEnabled(messageSubject, messagePriority);
}

uint64_t Settings::listenerControl(const string& action, const CListener& listener)
{
    const WriteLock lock(m_mutex);

    return m_listenerManager.control(action, listener);
}

uint64_t Settings::bridgeControl(const string& action, const CBridge& bridge)
{
    const WriteLock lock(m_mutex);

    return m_bridgeManager.control(action, bridge);
}

CPersistence Settings::persistenceControl(const string& action, const CPersistence& persistence)
{
    const WriteLock lock(m_mutex);

    if (action == "set")
    {
        // Read before anything is assigned. What came back may still carry the mask this method
        // sent out, meaning "the password is unchanged", and the password to put back is the one
        // on record - which the assignment below is about to overwrite.
        const String storedUri(m_persistence.m_redis_uri.asString());

        m_persistence = persistence;
        m_persistence.m_redis_uri = UriSecret::restored(String(persistence.m_redis_uri.asString()),
                                                        storedUri);
        saveConfigurationUnlocked();
    }

    // The password goes no further than this process. Bridge passwords have always been sent out
    // masked; a URI carrying one was sent out whole, which is the same secret with none of the care.
    CPersistence answer(m_persistence);
    answer.m_redis_uri = UriSecret::hidden(String(m_persistence.m_redis_uri.asString()));
    return answer;
}

CSSLKeys Settings::sslKeysControl(const string& action, const CSSLKeys& sslKeys)
{
    const WriteLock lock(m_mutex);

    if (action == "set")
    {
        m_connections.m_ssl_keys = sslKeys;
        saveConfigurationUnlocked();
    }

    return m_connections.m_ssl_keys;
}

void Settings::limitsControl(const string& action, CServerLimits& serverLimits, CQueueLimits& queueLimits)
{
    const WriteLock lock(m_mutex);

    if (action == "set")
    {
        if (!m_server_limits.isNull())
        {
            m_server_limits = serverLimits;
        }
        if (!m_queue_limits.isNull())
        {
            m_queue_limits = queueLimits;
        }
        saveConfigurationUnlocked();
    }
    else if (action == "get")
    {
        serverLimits = m_server_limits;
        queueLimits = m_queue_limits;
    }
}

void Settings::loggingControl(const std::string& action, CLogging& logging)
{
    const WriteLock lock(m_mutex);

    if (action == "set")
    {
        if (!logging.isNull())
        {
            m_logging = logging;
            saveConfigurationUnlocked();
        }
    }
    else if (action == "get")
    {
        logging = m_logging;
    }
}

CWebService Settings::serviceControl(const std::string& action, const CWebService& webService)
{
    const WriteLock lock(m_mutex);

    if (action == "set")
    {
        if (!webService.isNull())
        {
            // Only what the caller actually sent. A page that edits one field sends that field
            // alone, and taking the element whole would blank the rest - including the
            // certificate paths, which would drop the interface to plain HTTP at the next start.
            if (!webService.m_listener_port.isNull())
            {
                m_web_service.m_listener_port = webService.m_listener_port.asInteger();
            }
            if (!webService.m_encrypted.isNull())
            {
                m_web_service.m_encrypted = webService.m_encrypted.asBool();
            }
            if (!webService.m_certfile.asString().empty())
            {
                m_web_service.m_certfile = webService.m_certfile.asString();
            }
            if (!webService.m_keyfile.asString().empty())
            {
                m_web_service.m_keyfile = webService.m_keyfile.asString();
            }
            saveConfigurationUnlocked();
        }
    }
    return m_web_service;
}

namespace {

constexpr auto mqttProtocol = "MQTT";
constexpr auto mqttSslProtocol = "MQTT+SSL";
constexpr auto defaultRedisPort = 6379;
constexpr auto defaultListenerThreads = 4;
constexpr auto lowestUsablePort = 1024;
constexpr auto highestPort = 65535;

/**
 * @brief Read the configuration template.
 *
 * The text is left as it stands, placeholders and all: what is built from it goes back through
 * the normal load path, which is where ${ProgramCerts} and its like are resolved. Resolving them
 * here would write absolute paths into a configuration that is meant to stay portable.
 *
 * The template is edited as a document rather than through the generated configuration classes.
 * Loading into those and writing them out again is not lossless - an absent optional element with
 * an enumerated type comes back as an empty string, which then fails its own restriction - and a
 * template is exactly the file that should survive being read and written unchanged.
 *
 * @param templateFile      Template to read.
 * @param document          Receives the parsed template.
 */
void loadConfigurationTemplate(const filesystem::path& templateFile, xdoc::Document& document)
{
    error_code errorCode;
    if (!filesystem::exists(templateFile, errorCode))
    {
        throw Exception("The configuration template is missing: " + templateFile.string());
    }

    Buffer templateText;
    templateText.loadFromFile(templateFile);
    document.load(String(templateText));
}

/**
 * @brief A section of the configuration, created empty when the template has no such section.
 */
xdoc::SNode section(const xdoc::SNode& parent, const char* name)
{
    if (auto node = parent->findFirst(name, xdoc::SearchMode::ImmediateChild))
    {
        return node;
    }
    return parent->pushNode(name, xdoc::Node::Type::Object);
}

/**
 * @brief The listener serving a protocol, added when the template defines none.
 *
 * A template without an encrypted listener is unusual but not broken, and the setup screen asks
 * for both ports either way - so the missing one is created rather than silently ignored.
 *
 * @param listeners     Listener array from the template.
 * @param protocol      "MQTT" or "MQTT+SSL".
 * @return the listener to give the port to.
 */
xdoc::SNode listenerFor(const xdoc::SNode& listeners, const char* protocol)
{
    for (const auto& listener: listeners->nodes())
    {
        if (listener->getString("protocol") == protocol)
        {
            return listener;
        }
    }

    const auto listener = listeners->pushNode("listener", xdoc::Node::Type::Object);
    listener->set("name", String(protocol) == mqttProtocol ? "Not Encrypted" : "Encrypted");
    listener->set("protocol", String(protocol));
    listener->set("bind_ip", String("0.0.0.0"));
    listener->set("threads", defaultListenerThreads);
    listener->set("enable", true);
    return listener;
}

/**
 * @brief The port a protocol is served on in the template, or 0 when it defines no such listener.
 */
int templatePort(const xdoc::SNode& listeners, const char* protocol)
{
    for (const auto& listener: listeners->nodes())
    {
        if (listener->getString("protocol") == protocol)
        {
            return static_cast<int>(listener->getInteger("port"));
        }
    }
    return 0;
}

/**
 * @brief Split "host:port" into its parts.
 *
 * Parsed here rather than with Host, which resolves what it is given: a configuration naming a
 * machine that is not up, or not reachable from here, should still show its address.
 *
 * @param address       Address to split; the port may be absent.
 * @param port          Receives the port, left as the caller set it when the address carries none.
 * @return the host name, or empty when the address names none.
 */
String splitHostPort(const String& address, int& port)
{
    if (const auto portStart = address.rfind(':');
        portStart != String::npos)
    {
        if (const auto portNumber = String(address.substr(portStart + 1)).toInt();
            portNumber > 0)
        {
            port = portNumber;
        }
        return address.substr(0, portStart);
    }
    return address;
}

/**
 * @brief Split a Redis URI into the connection fields the setup screen shows.
 *
 * @param redisUri      URI as the configuration writes it, e.g. "redis://localhost:6379".
 * @param host          Receives the host name, or empty when the URI names none.
 * @param port          Receives the port, defaulted when the URI carries none.
 * @param database      Receives the Redis database number, defaulted to zero.
 */
void splitRedisUri(const String& redisUri, String& host, int& port, int& database)
{
    host = "";
    port = defaultRedisPort;
    database = 0;

    String address = redisUri;
    if (const auto schemeEnd = address.find("://");
        schemeEnd != String::npos)
    {
        address = address.substr(schemeEnd + 3);
    }
    // Split credentials away before parsing the host. The username is provided separately below;
    // the password is deliberately never returned to the setup screen.
    if (const auto credentialsEnd = address.rfind('@');
        credentialsEnd != String::npos)
    {
        address = address.substr(credentialsEnd + 1);
    }
    if (const auto pathStart = address.find('/');
        pathStart != String::npos)
    {
        const auto databaseText = address.substr(pathStart + 1);
        if (!databaseText.empty() && databaseText.size() <= 5 &&
            all_of(databaseText.begin(), databaseText.end(), [](const char c) { return isdigit(static_cast<unsigned char>(c)); }))
        {
            database = stoi(databaseText);
        }
        address = address.substr(0, pathStart);
    }

    host = splitHostPort(address, port);
}

/**
 * @brief The address other machines reach this node at, as the setup gives it.
 *
 * Empty means this machine's own name: right for a single server, and the thing to correct on
 * anything that other nodes connect to.
 *
 * @param setup         Settings the setup screen sent.
 * @return the address, never empty.
 */
String nodeHostOf(const CInitialSetup& setup)
{
    const auto nodeHost = String(setup.m_node_host.asString()).trim();
    return nodeHost.empty() ? thisHostName() : nodeHost;
}

/**
 * @brief Reject settings that would produce a server that cannot start.
 *
 * Checked before anything is written: a configuration replaced by an unusable one leaves the
 * server refusing to start with the settings it was replacing gone.
 */
void checkInitialSetup(const CInitialSetup& setup)
{
    if (!setup.m_redis_host.asString().empty() && !setup.m_redis_username.asString().empty() && setup.m_redis_password.asString().empty())
        throw Exception("Enter a Redis password with the username, or leave both credentials empty.");
    if (setup.m_admin_password.asString().empty())
    {
        throw Exception("The administrator password is required.");
    }
    PasswordHash::refuseIfWeak(setup.m_admin_password.asString());
    if (setup.m_cluster_password.asString().empty())
    {
        throw Exception("The cluster password is required.");
    }
    PasswordHash::refuseIfWeak(setup.m_cluster_password.asString());

    const auto checkPort = [](const String& name, const int port)
    {
        if (port < lowestUsablePort || port > highestPort)
        {
            throw Exception(name + " must be between " + to_string(lowestUsablePort) + " and " +
                            to_string(highestPort) + ".");
        }
    };

    const auto mqttPort = setup.m_mqtt_port.asInteger();
    const auto mqttSslPort = setup.m_mqtt_ssl_port.asInteger();
    const auto webServicePort = setup.m_web_service_port.asInteger();

    checkPort("The MQTT port", mqttPort);
    checkPort("The MQTT+SSL port", mqttSslPort);
    checkPort("The web interface port", webServicePort);

    if (mqttPort == mqttSslPort || mqttPort == webServicePort || mqttSslPort == webServicePort)
    {
        throw Exception("The MQTT, MQTT+SSL, and web interface ports must all differ.");
    }

    if (!setup.m_redis_host.asString().empty() &&
        (setup.m_redis_port.asInteger() < 1 || setup.m_redis_port.asInteger() > highestPort))
    {
        throw Exception("The Redis port must be between 1 and " + to_string(highestPort) + ".");
    }
    if (!setup.m_redis_host.asString().empty() &&
        (setup.m_redis_database.asInteger() < 0 || setup.m_redis_database.asInteger() > 16))
    {
        throw Exception("Choose a Redis database number between 0 and 16.");
    }

    // The accounts database, for users and cluster configuration. Empty means the default
    // SQLite, which is always reachable. A given URI means a shared database that must respond.
    const auto databaseUri = String(setup.m_database_uri.asString()).trim();
    if (!databaseUri.empty() && setup.m_redis_host.asString().empty())
    {
        throw Exception("A shared accounts database needs shared storage: provide the Redis host.");
    }

    // The ports are asked for separately and appended to this, so a host that carries one of its
    // own would produce an address with two. Said plainly rather than quietly stripped: someone
    // who typed a port meant something by it.
    if (const auto nodeHost = String(setup.m_node_host.asString()).trim();
        !nodeHost.empty())
    {
        if (nodeHost.find(':') != String::npos)
        {
            throw Exception("The node address must be a host name or address without a port: the "
                "ports are set above.");
        }
        if (nodeHost.find_first_of(" \t/") != String::npos)
        {
            throw Exception("The node address must be a host name or address, with nothing else "
                "in it.");
        }
    }
}

} // namespace

pair<filesystem::path, filesystem::path> Settings::nodeKeyFiles()
{
    const auto directory = DirectoryNames::certsDirectory();
    return {directory / "node.crt", directory / "node.key"};
}

filesystem::path Settings::peerCertificatesDirectory()
{
    return DirectoryNames::certsDirectory() / "peers";
}

filesystem::path Settings::peerCertificateAuthority(const String& peerName)
{
    const filesystem::path filename(string(peerName) + ".crt");
    if (!peerName.empty() && filename == filename.filename())
    {
        const auto peer = peerCertificatesDirectory() / filename;
        if (filesystem::is_regular_file(peer)) return peer;
    }
    return buildPeerCertificateBundle();
}

filesystem::path Settings::buildPeerCertificateBundle()
{
    const auto directory = peerCertificatesDirectory();

    error_code errorCode;
    if (!filesystem::exists(directory, errorCode))
    {
        return {};
    }

    Buffer bundle;
    for (const auto& entry: filesystem::directory_iterator(directory, errorCode))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".crt")
        {
            continue;
        }

        Buffer certificate;
        certificate.loadFromFile(entry.path());
        bundle.append(certificate);

        // Certificates concatenated without a separating newline read as one damaged block, and
        // OpenSSL stops at the first thing it cannot parse rather than saying so.
        if (bundle.size() > 0 && bundle.data()[bundle.size() - 1] != '\n')
        {
            bundle.append("\n", 1);
        }
    }

    if (bundle.empty())
    {
        return {};
    }

    // Named after what it holds. SPTK caches an SSL context by the names of its files and looks
    // at the files again only once a second, so a bundle rewritten under the same name could
    // still be checked against the certificates it held before - a peer just removed from the
    // trusted ones trusted a while longer. A new name is a new context, at once.
    const auto contentHash = std::hash<string_view>{}(string_view(bundle.c_str(), bundle.size()));
    const auto bundleName = format("peers-{:016x}.crt", contentHash);
    const auto bundleFile = DirectoryNames::certsDirectory() / bundleName;
    if (!filesystem::exists(bundleFile, errorCode))
    {
        bundle.saveToFile(bundleFile);
    }

    // Bundles of earlier trust sets are of no further use: a context already made from one has
    // read it.
    for (const auto& entry: filesystem::directory_iterator(DirectoryNames::certsDirectory(), errorCode))
    {
        const auto name = entry.path().filename().string();
        if (name != bundleName && name.starts_with("peers") && entry.path().extension() == ".crt")
        {
            filesystem::remove(entry.path(), errorCode);
        }
    }
    return bundleFile;
}

String Settings::nodeHostName() const
{
    const ReadLock lock(m_mutex);

    int        port = 0;
    const auto host = splitHostPort(m_cluster.m_this_node.m_host_port.asString(), port);

    // Whatever the configuration says is deliberate, including "localhost": a server that is only
    // ever reached from its own machine is a real arrangement, not a mistake to correct here.
    return host.empty() ? thisHostName() : host;
}

String Settings::cloneNodes(const filesystem::path& sourceFile, unsigned count)
{
    if (count == 0 || count > 65535) throw Exception("Node count must be between 1 and 65535.");
    const auto requested = DirectoryNames::resolveConfigurationFile(sourceFile);
    const auto oldDirectory = requested.parent_path();
    const auto parent = oldDirectory.parent_path();
    const auto first = parent / "xmq1";
    if ((oldDirectory.filename() != "xmq" && oldDirectory.filename() != "xmq1") ||
        requested.filename() != "xmq_server.conf" || filesystem::is_symlink(oldDirectory) ||
        filesystem::is_symlink(first))
        throw Exception("The source must be xmq or xmq1 in a real configuration directory.");
    const bool moveOriginal = !filesystem::is_regular_file(first / "xmq_server.conf");
    const auto source = moveOriginal ? requested : first / "xmq_server.conf";
    if (!filesystem::is_regular_file(source) || (moveOriginal && oldDirectory == first))
        throw Exception("Source configuration is missing: " + source.string());
    vector<unsigned> missing;
    for (unsigned index = 1; index <= count; ++index)
    {
        const auto destination = parent / ("xmq" + to_string(index));
        if (filesystem::is_regular_file(destination / "xmq_server.conf")) continue;
        if (filesystem::exists(destination))
            throw Exception("Destination directory exists without a configuration: " + destination.string());
        if (index > 1) missing.push_back(index);
    }
    if (!moveOriginal && missing.empty()) return "All requested configurations already exist; nothing changed.";
    Buffer content;
    content.loadFromFile(source);
    xdoc::Document configuration;
    configuration.load(content);
    CServerConfiguration settings;
    settings.load(configuration.root());
    int mqtt = 0, tls = 0;
    for (const auto& listener: settings.m_connections.m_listener)
    {
        const auto protocol = listener.m_protocol.asString();
        if (protocol == "MQTT" && !mqtt) mqtt = static_cast<int>(listener.m_port.asInteger());
        else if (protocol == "MQTT+SSL" && !tls) tls = static_cast<int>(listener.m_port.asInteger());
        else if (listener.m_enable.isNull() || listener.m_enable.asBool())
            throw Exception("Bulk cloning requires one MQTT and one MQTT+SSL listener, without additional enabled listeners.");
    }
    const auto web = settings.m_web_service.m_listener_port.asInteger();
    set<int64_t> ports;
    for (unsigned index = 1; index <= count; ++index)
        for (const auto base: {int64_t(mqtt), int64_t(tls), int64_t(web)})
        {
            const auto port = base + index - 1;
            if (base < 1024 || port > 65535 || !ports.insert(port).second)
                throw Exception("The requested node count creates invalid or overlapping port ranges.");
        }
    int ignoredPort = 0;
    const auto host = splitHostPort(settings.m_cluster.m_this_node.m_host_port.asString(), ignoredPort);
    const auto name = settings.m_cluster.m_this_node.m_node_name.asString();
    if (host.empty() || host.find_first_of(" \t\r\n/\\:") != String::npos || name.empty())
        throw Exception("The source must have a usable node name and advertised host.");

    // Rebase only path-valued strings, not arbitrary substrings in passwords or URLs.
    // The original UUID, certificates and database move intact with their directory.
    vector<pair<filesystem::path, Buffer>> originals;
    vector<pair<filesystem::path, Buffer>> rewritten;
    const auto rebase = [&](const auto& self, const xdoc::SNode& node) -> void
    {
        if (node->type() == xdoc::Node::Type::Text)
        {
            const string value(node->getString());
            for (const auto& [from, to]: vector<pair<string, string>> {
                    {oldDirectory.string(), first.string()},
                    {oldDirectory.generic_string(), first.generic_string()},
                    {string(DirectoryNames::sqliteUri(oldDirectory)), string(DirectoryNames::sqliteUri(first))}})
                if (value == from || (value.starts_with(from) && value.size() > from.size() &&
                    (value[from.size()] == '/' || value[from.size()] == '\\')))
                {
                    node->set(to + value.substr(from.size()));
                    break;
                }
        }
        for (const auto& child: node->nodes()) self(self, child);
    };
    if (moveOriginal) for (const auto& entry: filesystem::recursive_directory_iterator(oldDirectory))
    {
        const auto filename = entry.path().filename().string();
        if (!(filename.ends_with(".conf") || filename == "xmq_server.conf.template" ||
              filename == "xmq_server.conf.lastgood")) continue;
        if (entry.is_symlink()) throw Exception("Cannot relocate a symlinked configuration: " + entry.path().string());
        if (!entry.is_regular_file()) continue;
        Buffer original;
        original.loadFromFile(entry.path());
        xdoc::Document document;
        document.load(original);
        rebase(rebase, document.root());
        Buffer changed;
        document.exportTo(xdoc::DataFormat::JSON, changed, true);
        const auto relative = filesystem::relative(entry.path(), oldDirectory);
        originals.emplace_back(relative, original);
        rewritten.emplace_back(relative, changed);
    }
    if (moveOriginal) filesystem::rename(oldDirectory, first);
    vector<unsigned> created;
    try
    {
        for (const auto& [relative, changed]: rewritten) changed.saveToFile(first / relative);
        for (const auto index: missing)
        {
            CNodeSettings node;
            node.m_node_name = name + "-" + to_string(index);
            node.m_node_host = host;
            node.m_mqtt_port = static_cast<int64_t>(mqtt) + index - 1;
            node.m_mqtt_ssl_port = static_cast<int64_t>(tls) + index - 1;
            node.m_web_service_port = static_cast<int64_t>(web) + index - 1;
            const auto result = cloneNode(first / "xmq_server.conf", parent / ("xmq" + to_string(index)) / "xmq_server.conf", node);
            COUT(result);
            created.push_back(index);
        }
    }
    catch (...)
    {
        const auto failure = current_exception();
        // Only directories successfully created by this batch are removed.
        for (const auto index: created)
            filesystem::remove_all(parent / ("xmq" + to_string(index)));
        if (moveOriginal)
        {
            for (const auto& [relative, original]: originals) original.saveToFile(first / relative);
            filesystem::rename(first, oldDirectory);
        }
        rethrow_exception(failure);
    }
    return "Created " + to_string(created.size()) + " clone(s); existing configurations were preserved. "
           "The original is at " + first.string() + ". No servers were started.";
}

String Settings::cloneNode(const filesystem::path& sourceFile,
                           const filesystem::path& destinationFile,
                           const CNodeSettings& node)
{
    const auto source = DirectoryNames::resolveConfigurationFile(sourceFile);
    const auto destination = DirectoryNames::resolveConfigurationFile(destinationFile);
    const auto directory = destination.parent_path();
    const auto name = String(node.m_node_name.asString()).trim();
    const auto host = String(node.m_node_host.asString()).trim();
    if (name.empty() || host.empty() || host.find_first_of(" \t\r\n/\\:") != String::npos)
        throw Exception("A node name and a host without a port are required.");
    const auto mqtt = node.m_mqtt_port.asInteger();
    const auto tls = node.m_mqtt_ssl_port.asInteger();
    const auto web = node.m_web_service_port.asInteger();
    for (const auto value: {mqtt, tls, web})
        if (value < 1024 || value > 65535)
            throw Exception("Clone ports must be between 1024 and 65535.");
    if (mqtt == tls || mqtt == web || tls == web)
        throw Exception("Clone ports must be different.");
    if (!filesystem::is_regular_file(source))
        throw Exception("Source configuration does not exist: " + source.string());
    if (filesystem::exists(directory))
        throw Exception("The clone requires a new configuration directory: " + directory.string());

    // Read the file as a document: loading Settings could register the source in SQL,
    // migrate its users or rewrite it. None of those belongs to cloning.
    xdoc::Document document;
    Buffer content;
    content.loadFromFile(source);
    document.load(content);
    const auto root = document.root();
    CServerConfiguration validation;
    validation.load(root);
    const auto authentication = section(root, "authentication");
    String databaseUri = authentication->getString("database_uri");
    const bool localAccounts = databaseUri.empty() || databaseUri.toLowerCase().starts_with("sqlite");
    const auto newDatabase = directory / "xmq_users.db";
    const auto newDatabaseUri = DirectoryNames::sqliteUri(newDatabase);
    if (localAccounts)
        authentication->set("database_uri", newDatabaseUri);
    else
        authentication->remove("users");
    const auto cluster = section(root, "cluster");
    cluster->set("enabled", false);
    cluster->remove("nodes");
    cluster->remove("password");
    const auto identity = section(cluster, "this_node");
    identity->set("node_name", name);
    identity->set("host_port", host + ":" + to_string(tls));
    identity->set("encrypted", true);
    const auto connections = section(root, "connections");
    auto listeners = connections->findFirst("listener", xdoc::SearchMode::ImmediateChild);
    if (!listeners) listeners = connections->pushNode("listener", xdoc::Node::Type::Array);
    for (const auto& listener: listeners->nodes())
    {
        const auto protocol = listener->getString("protocol");
        if (protocol == mqttProtocol) listener->set("port", mqtt);
        else if (protocol == mqttSslProtocol) listener->set("port", tls);
        else if (listener->getBoolean("enable"))
            throw Exception("Clone requires explicit configuration of additional listener protocols.");
    }
    listenerFor(listeners, mqttProtocol)->set("port", mqtt);
    listenerFor(listeners, mqttSslProtocol)->set("port", tls);
    const auto cert = directory / "certs" / "node.crt";
    const auto key = directory / "certs" / "node.key";
    const auto keys = section(connections, "ssl_keys");
    keys->set("certfile", cert.string());
    keys->set("keyfile", key.string());
    // The source's trust bundle includes its identity and is not the clone's trust store.
    keys->remove("cafile");
    const auto webService = section(root, "web_service");
    webService->set("listener_port", web);
    webService->set("certfile", cert.string());
    webService->set("keyfile", key.string());
    section(root, "logging")->set("log_to", (directory / "xmq_server.log").string());
    section(root, "persistence")->set("clean_start", false);

    // Reserve the destination before writing anything. A failed clone removes only the
    // directory this invocation created; an existing node is never overwritten.
    filesystem::create_directories(directory.parent_path());
    if (!filesystem::create_directory(directory))
        throw Exception("The clone directory already exists: " + directory.string());
    try
    {
        filesystem::permissions(directory, filesystem::perms::owner_all,
                                filesystem::perm_options::replace);
        const auto copyFile = [&](const filesystem::path& from, const filesystem::path& to)
        {
            filesystem::copy_file(from, to);
            restrictToOwnerAndGroup(to);
        };
        if (localAccounts)
        {
            const auto oldDatabase = source.parent_path() / "xmq_users.db";
            if (!databaseUri.empty() || filesystem::is_regular_file(oldDatabase))
            {
                if (databaseUri.empty()) databaseUri = DirectoryNames::sqliteUri(oldDatabase);
                // SQLite's snapshot includes WAL contents; copying a live .db file does not.
                DatabaseConnectionPool pool(databaseUri, 1);
                const AutoDatabaseConnection connection(pool);
                Query snapshot(connection.connection(), "VACUUM INTO :destination");
                snapshot.param("destination") = newDatabase.string();
                snapshot.exec();
                restrictToOwnerAndGroup(newDatabase);
            }
            else if (filesystem::is_regular_file(source.parent_path() / "xmq_users.conf"))
                copyFile(source.parent_path() / "xmq_users.conf", directory / "xmq_users.conf");
        }
        // Extension settings travel with the node, but an explicit local account DB must
        // point to the snapshot. Libraries remain at their installed locations.
        const auto copyExtension = [&](const filesystem::path& from, const filesystem::path& to)
        {
            xdoc::Document extension;
            Buffer input;
            input.loadFromFile(from);
            extension.load(input);
            for (const auto& item: extension.root()->nodes("extensions"))
                if (localAccounts && item->getString("name") == "user-database")
                {
                    const auto settings = section(item, "settings");
                    settings->remove("database");
                    settings->remove("database_uri");
                }
            Buffer output;
            extension.exportTo(xdoc::DataFormat::JSON, output, true);
            output.saveToFile(to);
            restrictToOwnerAndGroup(to);
        };
        const auto extensions = source.parent_path() / "xmq_extensions.conf";
        if (filesystem::is_regular_file(extensions))
            copyExtension(extensions, directory / extensions.filename());
        const auto fragments = source.parent_path() / "xmq_extensions.d";
        if (filesystem::is_directory(fragments))
        {
            filesystem::create_directory(directory / fragments.filename());
            for (const auto& entry: filesystem::directory_iterator(fragments))
                if (entry.is_regular_file() && entry.path().extension() == ".conf")
                    copyExtension(entry.path(), directory / fragments.filename() / entry.path().filename());
        }
        const auto templateFile = source.parent_path() / "xmq_server.conf.template";
        if (filesystem::is_regular_file(templateFile))
            copyFile(templateFile, directory / templateFile.filename());
        const auto uuid = cluster::NodeIdentity::load(directory / cluster::NodeIdentity::FileName);
        restrictToOwnerAndGroup(directory / cluster::NodeIdentity::FileName);
        String description;
        SelfSignedCertificate::reissue(cert, key, host, description);
        Buffer output;
        document.exportTo(xdoc::DataFormat::JSON, output, true);
        output.saveToFile(destination);
        restrictToOwnerAndGroup(destination);
        return "Created standalone node '" + name + "' (" + uuid + ") at " + destination.string();
    }
    catch (...)
    {
        error_code ignored;
        filesystem::remove_all(directory, ignored);
        throw;
    }
}

filesystem::path Settings::configurationTemplatePath()
{
    return DirectoryNames::confDirectory() / "xmq_server.conf.template";
}

CInitialSetup Settings::initialSetup(const filesystem::path& templateFile) const
{
    xdoc::Document document;
    loadConfigurationTemplate(templateFile, document);

    const auto root = document.root();
    const auto listeners = section(root, "connections")->findFirst("listener", xdoc::SearchMode::ImmediateChild);

    // The address is offered only when the template actually turns persistence on. It always
    // carries one - "redis://localhost:6379" - and the shipped template has persistence off, so
    // reading the address regardless offered "localhost" on a page where an address means "switch
    // persistence on". Anyone who accepted the defaults and pressed Submit got a broker configured
    // against a Redis that is not there: it falls back to memory and says so in the log, which is
    // not where someone setting a server up is looking.
    //
    // The port is still offered, so that typing a host is all it takes to turn persistence on.
    const auto persistence = section(root, "persistence");
    String     redisHost;
    int        redisPort = defaultRedisPort;
    int        redisDatabase = 0;
    splitRedisUri(persistence->getString("redis_uri"), redisHost, redisPort, redisDatabase);
    if (!persistence->getBoolean("enabled"))
    {
        redisHost.clear();
    }

    CInitialSetup setup;
    const auto thisNode = section(section(root, "cluster"), "this_node");
    setup.m_node_name = thisHostName();

    // The template says "localhost", which is a placeholder rather than an answer: it is the one
    // address certain to be wrong for every other node in a cluster, because it names each node's
    // own machine. This machine's name is a better starting point, and the field can be set to
    // whatever actually resolves from outside.
    int        templateNodePort = 0;
    const auto templateNodeHost = splitHostPort(thisNode->getString("host_port"), templateNodePort);
    setup.m_node_host = (templateNodeHost.empty() || templateNodeHost == "localhost")
                            ? thisHostName()
                            : templateNodeHost;

    setup.m_mqtt_port = listeners ? templatePort(listeners, mqttProtocol) : 0;
    setup.m_mqtt_ssl_port = listeners ? templatePort(listeners, mqttSslProtocol) : 0;
    setup.m_web_service_port = static_cast<int>(section(root, "web_service")->getInteger("listener_port"));
    setup.m_redis_host = redisHost;
    setup.m_redis_port = redisPort;
    setup.m_redis_database = redisDatabase;
    if (!redisHost.empty())
        setup.m_redis_username = decodeRedisCredential(redisConfiguredUrl(persistence->getString("redis_uri")).username());
    setup.m_redis_password.clear();
    setup.m_database_uri = section(root, "authentication")->getString("database_uri");

    return setup;
}

void Settings::applyInitialSetup(const CInitialSetup& setup, const filesystem::path& templateFile)
{
    checkInitialSetup(setup);

    // Setting up another node against shared accounts must not reset everybody's
    // passwords. Verify the supplied credentials before changing any local settings.
    bool existingSharedAccounts = false;
    bool initializeSharedPasswords = false;
    const auto setupUri = setup.m_database_uri.asString();
    if (!setupUri.empty() && !setupUri.toLowerCase().starts_with("sqlite"))
    {
        auto pool = make_shared<DatabaseConnectionPool>(setupUri, 2);
        UserStore store(pool);
        store.createSchema();
        existingSharedAccounts = !store.users().empty();
        if (existingSharedAccounts)
        {
            initializeSharedPasswords = store.checkSetupPasswords(
                setup.m_admin_password.asString(), setup.m_cluster_password.asString());
        }
    }

    const WriteLock lock(m_mutex);

    xdoc::Document document;
    loadConfigurationTemplate(templateFile, document);

    const auto root = document.root();

    const auto connections = section(root, "connections");
    auto       listeners = connections->findFirst("listener", xdoc::SearchMode::ImmediateChild);
    if (!listeners)
    {
        listeners = connections->pushNode("listener", xdoc::Node::Type::Array);
    }
    listenerFor(listeners, mqttProtocol)->set("port", setup.m_mqtt_port.asInteger());
    listenerFor(listeners, mqttSslProtocol)->set("port", setup.m_mqtt_ssl_port.asInteger());

    section(root, "web_service")->set("listener_port", setup.m_web_service_port.asInteger());

    // The node's own cluster entry has to agree with both the TLS listener port and the
    // address other nodes are told to reach this one at. Taken from the setup rather than from the
    // template, which only ever says "localhost" - an address that resolves on every machine and
    // reaches the wrong server on all but one of them.
    const auto thisNode = section(section(root, "cluster"), "this_node");
    thisNode->set("node_name", setup.m_node_name.asString());
    const auto nodeHost = nodeHostOf(setup);
    thisNode->set("host_port", nodeHost + ":" + to_string(setup.m_mqtt_ssl_port.asInteger()));
    thisNode->set("encrypted", true);

    // No Redis address means no Redis: the server is set up to keep its state in memory, which
    // is a working configuration rather than a broken one.
    const auto persistence = section(root, "persistence");
    if (const auto redisHost = setup.m_redis_host.asString();
        redisHost.empty())
    {
        persistence->set("enabled", false);
    }
    else
    {
        const String password = setup.m_redis_password.asString();
        String username = setup.m_redis_username.asString();
        if (username.empty() && !password.empty()) username = "default";
        const String credentials = username.empty() && password.empty() ? String() :
            String(encodeRedisCredential(username)) + ":" + encodeRedisCredential(password) + "@";
        persistence->set("redis_uri", "redis://" + credentials + redisHost + ":" +
                                      to_string(setup.m_redis_port.asInteger()) + "/" +
                                      to_string(setup.m_redis_database.asInteger()));
        persistence->set("enabled", true);
    }

    // A shared accounts database registers a node for discovery, not cluster admission.
    // Enable on the first node or accepting an invitation is what enables membership.
    section(root, "cluster")->set("enabled", false);
    section(root, "cluster")->remove("password");
    const auto setupDatabaseUri = setup.m_database_uri.asString();
    if (!setupDatabaseUri.empty())
    {
        section(root, "authentication")->set("database_uri", setupDatabaseUri);
    }

    // A certificate of this installation's own, made out to the address just given. Issued here
    // rather than left to the next start for two reasons: this is the only moment the address is
    // known - a certificate written at startup can only name the machine - and it is the moment
    // an installation is being made distinct from every other one, which a certificate shipped
    // with the package is not.
    const auto [certificateFile, privateKeyFile] = nodeKeyFiles();
    String     certificateDescription;
    SelfSignedCertificate::reissue(certificateFile, privateKeyFile, nodeHost, certificateDescription);

    const auto webService = section(root, "web_service");
    webService->set("certfile", certificateFile.string());
    webService->set("keyfile", privateKeyFile.string());

    const auto brokerKeys = section(connections, "ssl_keys");
    brokerKeys->set("certfile", certificateFile.string());
    brokerKeys->set("keyfile", privateKeyFile.string());

    Buffer newConfiguration;
    document.exportTo(xdoc::DataFormat::JSON, newConfiguration, true);

    // Through the normal load path, so the new configuration is subject to exactly the checks a
    // configuration read at startup is - and is rejected here, while the old one is still on
    // disk, rather than at the next start.
    loadConfigurationUnlocked(newConfiguration);

    // A new or local accounts store is initialised with the supplied passwords. Existing
    // shared accounts belong to all nodes and must survive another node's setup unchanged.
    if (!existingSharedAccounts)
        resetUsersUnlocked(setup.m_admin_password.asString(), setup.m_cluster_password.asString());
    if (m_accountsPool)
    {
        UserStore store(m_accountsPool);
        if (initializeSharedPasswords)
        {
            store.initializeSetupPasswords(setup.m_admin_password.asString(), setup.m_cluster_password.asString(),
                                          passwordIterationsUnlocked());
            const auto [defaultGroup, administratorsGroup] = store.createStandardGroups();
            const auto admin = store.findUser("admin");
            store.addUserToGroup(admin->m_id, defaultGroup);
            store.addUserToGroup(admin->m_id, administratorsGroup);
            store.addUserToGroup(store.findUser("cluster")->m_id, defaultGroup);
            m_userManager.reload();
        }
        store.setClusterSecret(setup.m_cluster_password.asString());
        m_cluster.m_password = "";
    }

    // Explicit setup replaces this node's settings, including an older SQL row read by load.
    // SQL discovery must not turn a freshly initialised node back into a cluster member.
    m_cluster.m_enabled = false;
    m_cluster.m_this_node.m_node_name = setup.m_node_name.asString();
    m_cluster.m_this_node.m_host_port = nodeHost + ":" + to_string(setup.m_mqtt_ssl_port.asInteger());
    m_persistence.m_enabled = persistence->getBoolean("enabled");
    m_persistence.m_redis_uri = persistence->getString("redis_uri");
    if (m_accountsPool && !m_configurationPath.empty())
    {
        ClusterStore store(m_accountsPool);
        if (store.isClusterDatabase())
        {
            const auto id = cluster::NodeIdentity::load(filesystem::path(m_configurationPath).replace_filename(cluster::NodeIdentity::FileName));
            if (auto row = store.node(id))
            {
                row->m_name = m_cluster.m_this_node.m_node_name.asString();
                row->m_hostPort = m_cluster.m_this_node.m_host_port.asString();
                row->m_redisUri = m_persistence.m_redis_uri.asString();
                row->m_clusterEnabled = false;
                row->m_state = "Standalone";
                row->m_lastError = "";
                store.updateNode(*row);
                store.clearInvitation(id);
                m_clusterConfigurationError = "";
            }
        }
    }

    // Written unconditionally: loadConfigurationUnlocked() saves only what it had to correct,
    // and here the whole file is the change.
    saveConfigurationUnlocked();
}

CNodeSettings Settings::nodeSettings(const bool hideSecrets) const
{
    const ReadLock lock(m_mutex);
    CNodeSettings values;
    values.m_node_name = m_cluster.m_this_node.m_node_name;
    int port = 0;
    values.m_node_host = splitHostPort(m_cluster.m_this_node.m_host_port.asString(), port);
    if (values.m_node_host.asString().empty()) values.m_node_host = thisHostName();
    values.m_mqtt_port = 1883;
    values.m_mqtt_ssl_port = 8883;
    bool tcp = false, tls = false;
    for (const auto& listener: m_connections.m_listener)
    {
        if (!tcp && listener.m_protocol.asString() == "MQTT") { values.m_mqtt_port = listener.m_port; tcp = true; }
        if (!tls && listener.m_protocol.asString() == "MQTT+SSL") { values.m_mqtt_ssl_port = listener.m_port; tls = true; }
    }
    values.m_web_service_port = m_web_service.m_listener_port;
    const string redis = m_persistence.m_enabled.asBool() ? m_persistence.m_redis_uri.asString() : "";
    const string database = userDatabaseUriUnlocked();
    values.m_redis_uri = hideSecrets ? UriSecret::hidden(redis) : redis;
    values.m_database_uri = hideSecrets ? UriSecret::hidden(database) : database;
    values.m_cluster_enabled = m_cluster.m_enabled.asBool();
    values.m_clean_start = m_persistence.m_clean_start.asBool();
    return values;
}

pair<bool, String> Settings::nodeSettingsStatus() const
{
    const ReadLock lock(m_mutex);
    return {m_nodeSettingsPending, m_nodeSettingsMessage};
}

void Settings::nodeSettingsStatus(const bool pending, const String& message)
{
    const WriteLock lock(m_mutex);
    m_nodeSettingsPending = pending;
    m_nodeSettingsMessage = message;
}

std::function<void()> Settings::saveNodeSettings(const CNodeSettings& values)
{
    const WriteLock lock(m_mutex);
    if (m_configurationPath.empty()) throw Exception("This node has no configuration file to update.");
    Buffer original;
    original.loadFromFile(m_configurationPath);
    Buffer current;
    saveConfigurationUnlocked(current);
    xdoc::Document document;
    document.load(String(current));
    const auto root = document.root();
    const auto node = section(section(root, "cluster"), "this_node");
    node->set("node_name", values.m_node_name.asString());
    node->set("host_port", values.m_node_host.asString() + ":" + to_string(values.m_mqtt_ssl_port.asInteger()));
    const auto listeners = section(root, "connections")->findFirst("listener");
    if (!listeners) throw Exception("The node has no listener configuration.");
    listenerFor(listeners, mqttProtocol)->set("port", values.m_mqtt_port.asInteger());
    listenerFor(listeners, mqttSslProtocol)->set("port", values.m_mqtt_ssl_port.asInteger());
    section(root, "web_service")->set("listener_port", values.m_web_service_port.asInteger());
    section(root, "persistence")->set("enabled", !values.m_redis_uri.asString().empty());
    section(root, "persistence")->set("redis_uri", values.m_redis_uri.asString());
    section(root, "authentication")->set("database_uri", values.m_database_uri.asString());
    const auto id = cluster::NodeIdentity::load(filesystem::path(m_configurationPath).replace_filename(cluster::NodeIdentity::FileName));
    auto pool = make_shared<DatabaseConnectionPool>(values.m_database_uri.asString(), 2);
    ClusterStore store(pool);
    const bool shared = store.isClusterDatabase();
    optional<ClusterStore::Node> previous;
    if (shared)
    {
        store.createSchema();
        previous = store.node(id);
        if (previous && (previous->m_state == "Removed" || previous->m_state == "Retired"))
            throw Exception("This node's configuration in the target database was removed or retired.");
    }
    const auto path = m_configurationPath;
    const String oldConfiguration(original);
    const auto rollback = [pool, shared, previous, id, path, oldConfiguration] {
        // Restore the file even when restoring the database fails.
        saveAtomically(path, Buffer(oldConfiguration));
        if (shared)
        {
            ClusterStore target(pool);
            if (previous) target.updateNode(*previous);
            else target.removeNode(id);
        }
    };
    try
    {
        if (shared)
        {
            auto updated = previous.value_or(ClusterStore::Node());
            updated.m_nodeId = id;
            updated.m_name = values.m_node_name.asString();
            updated.m_hostPort = values.m_node_host.asString() + ":" + to_string(values.m_mqtt_ssl_port.asInteger());
            updated.m_databaseUri = values.m_database_uri.asString();
            updated.m_redisUri = values.m_redis_uri.asString();
            updated.m_clusterEnabled = m_cluster.m_enabled.asBool();
            if (!previous) { updated.m_state = "Standalone"; updated.m_leaseSeconds = m_cluster.m_lease_seconds.asInteger(); updated.m_links = m_cluster.m_links.asInteger(); }
            if (previous) store.updateNode(updated); else store.insertNode(updated);
        }
        Buffer replacement;
        document.exportTo(xdoc::DataFormat::JSON, replacement, true);
        const auto temporary = path.string() + ".settings.tmp";
        replacement.saveToFile(temporary);
        restrictToOwnerAndGroup(temporary);
        filesystem::rename(temporary, path);
    }
    catch (...) { rollback(); throw; }
    return rollback;
}

void Settings::resetUsersUnlocked(const String& administratorPassword, const String& clusterPassword)
{
    // Replaced through the user manager's own load, which clears what it holds and takes the list
    // as given. Going through addUser()/removeUser() instead would fire the change callback for
    // each one, and that callback comes back in here for the lock this is already holding.
    m_authentication.m_users = defaultUsers(administratorPassword, clusterPassword);
    m_cluster.m_password = clusterPassword;
    m_userManager.loadConfiguration(m_authentication, m_changed);
    // Held by the manager now, and written to wherever the accounts live - the database when there
    // is one, the users file otherwise. The database has to be told to drop what it had: this
    // screen replaces the accounts, and what a restart reads is the database.
    m_authentication.m_users.clear();
    m_userManager.replaceStoredAccounts();
    saveUsersUnlocked();
}

const char* Settings::administratorUsername()
{
    return "admin";
}

bool Settings::eventEnabled(const LogSubject subject) const
{
    const ReadLock lock(m_mutex);

    // Publish and ack are the two that happen per message; everything else happens once per session
    // or per subscription. That is the whole of the rule, and it is why the default is not one
    // value for all of them.
    const auto byDefault = subject != LogSubject::Publish && subject != LogSubject::Ack;

    // Found by name rather than by a switch over ten subjects, the way the log levels are read.
    // Not every subject has a field - session_errors and storage_events have none - and getField
    // answers that by throwing, so the absence is caught here rather than assumed away. A subject
    // with no field keeps its default: a missing field means "not said", never "no".
    try
    {
        const auto* field = dynamic_cast<const WSBool*>(m_events.getField(to_string(subject)));
        if (field == nullptr || field->isNull())
        {
            return byDefault;
        }
        return field->asBool();
    }
    catch (const Exception&)
    {
        return byDefault;
    }
}

bool Settings::administratorPasswordSet() const
{
    return m_userManager.hasPassword(administratorUsername());
}

String Settings::checkConfiguration(const filesystem::path& configurationFile, Strings& warnings)
{
    try
    {
        error_code errorCode;
        if (!filesystem::exists(configurationFile, errorCode))
        {
            return "The configuration file doesn't exist: " + configurationFile.string();
        }

        Buffer config;
        config.loadFromFile(configurationFile);

        // The same path resolution a start does, so a placeholder cannot make a valid file look
        // invalid or the other way round.
        const auto configString = expandConfigurationText(config, DirectoryNames::logsDirectory() / "xmq_server.log");

        xdoc::Document document;
        document.load(configString);

        CServerConfiguration parsed;
        parsed.load(document.root());

        validateConfigurationValues(parsed, warnings);
        collectUnknownKeys(parsed, document.root(), "", warnings);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

filesystem::path Settings::lastGoodConfigurationPath(const filesystem::path& configurationFile)
{
    return filesystem::path(configurationFile.string() + ".lastgood");
}

String Settings::createConfiguration(const filesystem::path& configurationFile, const filesystem::path& fallbackTemplate)
{
    error_code errorCode;
    if (filesystem::exists(configurationFile, errorCode))
    {
        return {};
    }

    if (const auto directory = configurationFile.parent_path();
        !directory.empty())
    {
        filesystem::create_directories(directory, errorCode);
    }

    // A template beside the configuration comes first: an installation that ships one has said
    // what a starting configuration looks like on this machine, which beats anything built in.
    auto templateFile = configurationFile;
    templateFile += ".template";
    if (!filesystem::exists(templateFile, errorCode))
    {
        templateFile = fallbackTemplate;
    }

    if (filesystem::exists(templateFile, errorCode))
    {
        Buffer templateText;
        templateText.loadFromFile(templateFile);
        saveAtomically(configurationFile, templateText);
        return "copied from " + templateFile.string();
    }

    const Buffer defaultConfiguration(defaultConfigurationText());
    saveAtomically(configurationFile, defaultConfiguration);
    return "built-in defaults";
}

String Settings::resetConfiguration(const filesystem::path& configurationFile, const filesystem::path& fallbackTemplate)
{
    String replaced;

    error_code errorCode;
    if (filesystem::exists(configurationFile, errorCode))
    {
        auto previousFile = configurationFile;
        previousFile += ".old";
        filesystem::rename(configurationFile, previousFile, errorCode);
        if (errorCode)
        {
            throw Exception("Can't move " + configurationFile.string() + " aside: " + errorCode.message());
        }
        replaced = "the previous one is now " + previousFile.string() + ", ";
    }

    return replaced + createConfiguration(configurationFile, fallbackTemplate);
}

String Settings::setAccountPassword(const filesystem::path& configurationFile, const filesystem::path& usersFile,
                                    const String&           username, const String&                    password)
{
    Settings settings;
    settings.loadConfiguration(configurationFile, usersFile);

    auto& users = settings.userManager();
    if (!users.hasUser(username))
    {
        throw Exception("There is no account named '" + username + "' in " +
                        settings.usersPathUnlocked().string() + ".");
    }

    // The plain password goes in and the token is derived on the way to storage, which is what
    // modifyUser() does with every account the interface saves - so an account given its password
    // here is stored exactly as one given it through the interface.
    auto account = users.findUser(username);
    account.m_password = password;
    users.modifyUser(account);

    // Say where it went. The accounts file is only one of the two places: with a database the
    // password never touches xmq_users.conf, and naming that file anyway sent the operator looking
    // for a change that was not there.
    const auto target = settings.m_accountsInDatabase
                            ? String("the database ") + UriSecret::hidden(settings.userDatabaseUriUnlocked())
                            : String(settings.usersPathUnlocked().string());
    return "password set for '" + username + "' in " + target;
}
