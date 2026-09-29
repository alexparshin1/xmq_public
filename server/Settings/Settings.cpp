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
#include "common/DirectoryNames.h"
#include "common/HostName.h"
#include "server/SelfSignedCertificate.h"
#include "PasswordHash.h"
#include "UriSecret.h"
#include "service/CUsers.h"

#include <openssl/rand.h>
#include <ranges>
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
    static constexpr size_t        passwordLength = 24;
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
WSArray<CUser> defaultUsers(const String& administratorPassword = "")
{
    WSArray<CUser> users("users");
    users.push_back(makeUser(Settings::administratorUsername(), administratorPassword.c_str()));
    users.push_back(makeUser(clusterUsername, generatedClusterPassword().c_str()));
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
    "max_size": 50000,
    "max_inflight_messages": 32768
  },
  "server_limits": {
    "send_threads": 3,
    "receive_threads": 3,
    "max_topic_alias": 128
  },
  "authentication": {
    "allow_anonymous": false
  },
  "persistence": {
    "redis_uri": "redis://localhost:6379",
    "clean_start": true,
    "enabled": false,
    "max_redis_connections": 32,
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
      "host_port": "localhost:1883"
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
    constexpr auto defaultInflightMessagesLimit = 16;

    CListener defaultListener;
    defaultListener.m_protocol = "MQTT";
    defaultListener.m_port = defaultMqttPort;
    defaultListener.m_bind_ip = "0.0.0.0";
    defaultListener.m_threads = defaultListenerThreads;
    m_connections.m_listener.push_back(defaultListener);

    m_queue_limits.m_max_size = defaultQueueSizeLimit;
    m_queue_limits.m_max_inflight_messages = defaultInflightMessagesLimit;

    initialize();
}

namespace {
String replacePathDelimiter(const filesystem::path& path)
{
    return String(path.string()).replace("\\\\", "/");
}
}

void Settings::loadConfigurationUnlocked(const Buffer& config)
{
    String     configString(config);
    const auto logTo = filesystem::path(m_logging.m_log_to.asString().c_str());
    configString = configString
                   .replace(R"(\$\{ProgramData\})", replacePathDelimiter(DirectoryNames::confDirectory()))
                   .replace(R"(\$\{ProgramCerts\})", replacePathDelimiter(DirectoryNames::certsDirectory()))
                   .replace(R"(\$\{ProgramTemp\})", replacePathDelimiter(DirectoryNames::tempDirectory()))
                   .replace(R"(\$\{ProgramLogs\})", replacePathDelimiter(logTo));

    xdoc::Document document;
    document.load(configString);

    // Noted before the document is consumed: a configuration carrying an accounts element - even
    // an empty one left by an earlier version - is rewritten without it, so the element does not
    // linger in files that have already been migrated.
    const auto authenticationNode = document.root()->findFirst("authentication");
    const auto hasUsersElement = authenticationNode && authenticationNode->findFirst("users");

    WSComplexType::load(document.root());

    initialize();
    if (hasUsersElement)
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

    m_bridgeManager.initialize(m_changed);
    m_listenerManager.initialize(m_changed);

    // "auto" and 0 both mean "decide from the hardware", and anything else has to be a number in
    // range. The schema already refuses everything that is neither, so this is about the range.
    const auto checkThreadSetting = [](const String& value, const char* name)
    {
        const auto text = String(value).trim().toLowerCase();
        if (text.empty() || text == "auto" || text == "0")
        {
            return;
        }
        constexpr auto minThreads = 1;
        constexpr auto maxThreads = 256;
        if (const auto count = text.toInt();
            count < minThreads || count > maxThreads)
        {
            throw invalid_argument(String("The server_limits.") + name +
                                   " setting should be \"auto\", 0, or a number in range [1..256]");
        }
    };

    checkThreadSetting(m_server_limits.m_send_threads.asString(), "send_threads");
    checkThreadSetting(m_server_limits.m_receive_threads.asString(), "receive_threads");
    checkThreadSetting(m_server_limits.m_delivery_threads.asString(), "delivery_threads");

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

void Settings::updateUsers()
{
    const WriteLock lock(m_mutex);

    // Only allow_anonymous belongs in the configuration file; the accounts go to the users file.
    const auto allowAnonymous = m_userManager.isAllowAnonymous();
    m_authentication.m_allow_anonymous = allowAnonymous;
}

void Settings::saveConfigurationUnlocked(Buffer& config) const
{
    xdoc::Document document;
    WSComplexType::unload(document.root());

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

} // namespace

void Settings::saveConfigurationUnlocked() const
{
    if (m_configurationPath.empty())
    {
        return;
    }

    Buffer config;
    saveConfigurationUnlocked(config);
    config.saveToFile(m_configurationPath);
    restrictToOwnerAndGroup(m_configurationPath);
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
        auto             store = std::make_shared<UserStore>(std::make_shared<DatabaseConnectionPool>(uri, static_cast<unsigned>(connections)));
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
        CERR("Cannot open the accounts database " << UriSecret::hidden(uri) << ": " << e.what()
            << ". The accounts read from the file are used instead.");
    }
}

String Settings::clusterPassword() const
{
    const ReadLock lock(m_mutex);

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
    usersConfig.saveToFile(usersPath);
    restrictToOwnerAndGroup(usersPath);
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
 * @brief Split a Redis URI into the host and port the setup screen shows.
 *
 * @param redisUri      URI as the configuration writes it, e.g. "redis://localhost:6379".
 * @param host          Receives the host name, or empty when the URI names none.
 * @param port          Receives the port, defaulted when the URI carries none.
 */
void splitRedisUri(const String& redisUri, String& host, int& port)
{
    host = "";
    port = defaultRedisPort;

    String address = redisUri;
    if (const auto schemeEnd = address.find("://");
        schemeEnd != String::npos)
    {
        address = address.substr(schemeEnd + 3);
    }
    // Credentials are dropped: the setup screen asks for an address only, and showing a password
    // in a field that will not be sent back would lose it the moment the form is submitted.
    if (const auto credentialsEnd = address.rfind('@');
        credentialsEnd != String::npos)
    {
        address = address.substr(credentialsEnd + 1);
    }
    if (const auto pathStart = address.find('/');
        pathStart != String::npos)
    {
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
    if (setup.m_admin_password.asString().empty())
    {
        throw Exception("The administrator password is required.");
    }
    PasswordHash::refuseIfWeak(setup.m_admin_password.asString());

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

    const auto bundleFile = DirectoryNames::certsDirectory() / "peers.crt";
    bundle.saveToFile(bundleFile);
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
    splitRedisUri(persistence->getString("redis_uri"), redisHost, redisPort);
    if (!persistence->getBoolean("enabled"))
    {
        redisHost.clear();
    }

    CInitialSetup setup;
    const auto    thisNode = section(section(root, "cluster"), "this_node");
    setup.m_node_name = thisNode->getString("node_name");

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

    return setup;
}

void Settings::applyInitialSetup(const CInitialSetup& setup, const filesystem::path& templateFile)
{
    checkInitialSetup(setup);

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

    // The node's own cluster entry has to agree with both the port MQTT is now served on and the
    // address other nodes are told to reach this one at. Taken from the setup rather than from the
    // template, which only ever says "localhost" - an address that resolves on every machine and
    // reaches the wrong server on all but one of them.
    const auto thisNode = section(section(root, "cluster"), "this_node");
    thisNode->set("node_name", setup.m_node_name.asString());
    const auto nodeHost = nodeHostOf(setup);
    thisNode->set("host_port", nodeHost + ":" + to_string(setup.m_mqtt_port.asInteger()));

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
        persistence->set("redis_uri", "redis://" + redisHost + ":" +
                                      to_string(setup.m_redis_port.asInteger()));
        persistence->set("enabled", true);
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

    // The accounts go back to their defaults along with everything else, and after the load that
    // has just read the old ones back in. A setup that left the previous installation's clients
    // able to connect would not be the fresh start the page offers - and it cannot lock anyone
    // out, because the administrator password is asked for on the same page.
    resetUsersUnlocked(setup.m_admin_password.asString());

    // Written unconditionally: loadConfigurationUnlocked() saves only what it had to correct,
    // and here the whole file is the change.
    saveConfigurationUnlocked();
}

void Settings::resetUsersUnlocked(const String& administratorPassword)
{
    // Replaced through the user manager's own load, which clears what it holds and takes the list
    // as given. Going through addUser()/removeUser() instead would fire the change callback for
    // each one, and that callback comes back in here for the lock this is already holding.
    m_authentication.m_users = defaultUsers(administratorPassword);
    captureClusterSecretUnlocked();
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
        templateText.saveToFile(configurationFile);
        return "copied from " + templateFile.string();
    }

    const Buffer defaultConfiguration(defaultConfigurationText());
    defaultConfiguration.saveToFile(configurationFile);
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

    return "password set for '" + username + "' in " + settings.usersPathUnlocked().string();
}
