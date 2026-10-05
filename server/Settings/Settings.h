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

#include "BridgeManager.h"
#include "ListenerManager.h"
#include "LogSubject.h"
#include "LogSubjectPriorities.h"
#include "UserManager.h"
#include "service/CInitialSetup.h"
#include "service/CServerConfiguration.h"
#include <sptk5/cutils>

namespace xmq {

class XMQ_EXPORT Settings final : public CServerConfiguration
{
    friend class Server;

public:
    /**
     * @brief Constructor.
     */
    Settings();

    /**
     * @brief Destructor.
     */
    ~Settings() override = default;

    void loadConfiguration(const sptk::Buffer& config);
    void loadConfiguration(const std::filesystem::path& configurationFile,
                           const std::filesystem::path& usersFile = {});

    void              setLogPriority(sptk::LogPriority logPriority);
    sptk::LogPriority getLogPriority() const;

    void setLogSubjectPriority(LogSubject messageSubject, sptk::LogPriority messagePriority);
    void setLogSubjectsPriority(std::initializer_list<LogSubject> messageSubjects, sptk::LogPriority priority);
    void setLogSubjectsPriority(const std::vector<LogSubject>& messageSubjects, sptk::LogPriority messagePriority);

    /**
     * @brief Check if the log subject is enabled for the message priority.
     * @param messageSubject    Message subject.
     * @param messagePriority   Message priority.
     * @return true if the log subject is enabled.
     */
    [[nodiscard]] bool logSubjectEnabled(LogSubject messageSubject, sptk::LogPriority messagePriority) const;

    [[maybe_unused]] bool changed() const
    {
        const sptk::ReadLock lock(m_mutex);
        return m_changed;
    }
    /// Path of the configuration file this was loaded from; empty when it came from a buffer.
    [[nodiscard]] std::filesystem::path configurationPath() const
    {
        const sptk::ReadLock lock(m_mutex);
        return m_configurationPath;
    }


    UserManager& userManager()
    {
        return m_userManager;
    }

    uint64_t     bridgeControl(const std::string& action, const CBridge& bridge);
    uint64_t     listenerControl(const std::string& action, const CListener& listener);
    CPersistence persistenceControl(const std::string& action, const CPersistence& persistence);
    CSSLKeys     sslKeysControl(const std::string& action, const CSSLKeys& sslKeys);
    void         limitsControl(const std::string& action, CServerLimits& serverLimits, CQueueLimits& queueLimits);
    void         loggingControl(const std::string& action, CLogging& logging);
    CWebService  serviceControl(const std::string& action, const CWebService& webService);

    /**
     * @brief The address other machines reach this node at.
     *
     * Taken from the node's own cluster entry, which is the one place the configuration records
     * how this server is reached rather than how it listens. It is what a certificate issued here
     * has to be made out to: a certificate naming a host nobody connects by proves nothing to the
     * node that connects by another.
     *
     * @return the address, or this machine's own name when the configuration records none.
     */
    [[nodiscard]] sptk::String nodeHostName() const;

    /**
     * @brief Where this node's own certificate and key live.
     *
     * One pair for the whole server: it is what this node is, and a node has one identity. The
     * configuration interface and the MQTT+SSL listeners both start out pointed at it, and either
     * can be pointed elsewhere afterwards.
     *
     * @return the certificate path and the private key path.
     */
    [[nodiscard]] static std::pair<std::filesystem::path, std::filesystem::path> nodeKeyFiles();

    /**
     * @brief Where the certificates of the nodes this one trusts are kept, one file each.
     * @return the directory, which need not exist.
     */
    [[nodiscard]] static std::filesystem::path peerCertificatesDirectory();

    /**
     * @brief The trusted peers, gathered into the single file OpenSSL is given.
     *
     * Rebuilt from the directory rather than edited: a bundle assembled every time cannot drift
     * from the certificates it is supposed to contain. Its name follows its content, so a change
     * of the trusted peers is a new file - and a new SSL context - at once.
     *
     * @return the bundle path, or empty when no peer is trusted.
     */
    [[nodiscard]] static std::filesystem::path buildPeerCertificateBundle();

    /**
     * @brief The installed configuration template, which a new configuration is built from.
     *
     * Beside the configuration file it is a template for, and written by the installer rather
     * than by the server: it is what the settings looked like before anyone edited them.
     */
    [[nodiscard]] static std::filesystem::path configurationTemplatePath();

    /**
     * @brief The values a configuration built from the template would start with.
     *
     * Read from the template rather than from the running configuration: this is what the setup
     * screen offers, and offering the values already in use would defeat the point of it.
     *
     * @param templateFile      Template to read; the installed one by default.
     * @return the settings the setup screen asks about.
     * @throws sptk::Exception when the template is missing or does not parse.
     */
    [[nodiscard]] CInitialSetup initialSetup(const std::filesystem::path& templateFile = configurationTemplatePath()) const;

    /**
     * @brief Replace the whole configuration with the template, adjusted by the given settings.
     *
     * Everything the setup screen does not ask about goes back to its template value, so this
     * discards bridges, listeners, limits, and logging settings alike.
     *
     * The accounts go with them: what is left is the 'admin' account, with the password given,
     * and the account cluster peers connect as. Nobody is locked out by that, because the
     * password is part of what is being applied.
     *
     * @param setup             Administrator password, node name, ports, and optional Redis address.
     * @param templateFile      Template to build from; the installed one by default.
     * @throws sptk::Exception when the settings are not usable, or the template is missing.
     */
    void applyInitialSetup(const CInitialSetup& setup,
                           const std::filesystem::path& templateFile = configurationTemplatePath());

    /**
     * @brief The account name the interface is administered through.
     */
    [[nodiscard]] static const char* administratorUsername();

    /**
     * @brief Whether the administrator account has a password yet.
     *
     * A server installed and never set up has none. Until one is set the interface is bound to
     * the loopback address and admits the administrator without a password, so that whoever
     * installed the server can set one; everything else about the account is refused, including
     * every MQTT listener. Setting the password through the setup page is what ends that state.
     */
    /**
     * @brief Whether events of this subject reach observer extensions.
     *
     * A subject the configuration does not mention keeps its default, and the defaults are not all
     * the same: connect, disconnect, subscribe and unsubscribe happen once per session or per
     * subscription and are on, while publish and ack happen per message and are off. Raising an
     * event costs the thread that raised it three string copies into a bounded queue - nothing once
     * per session, and the broker's hottest path once per message. An installation that wants them
     * says so and accepts the cost.
     */
    [[nodiscard]] bool eventEnabled(LogSubject subject) const;

    [[nodiscard]] bool administratorPasswordSet() const;

    /**
     * @brief Write a starting configuration to this path, when there is not one there already.
     *
     * Created rather than refused. The configuration interface is how a server is configured, and
     * a missing configuration file is precisely the moment someone needs it: refusing to start
     * leaves them writing JSON by hand to reach the page that exists to save them from that.
     *
     * The content is taken from a template beside the requested file, then from the installed
     * template, and failing both from built-in defaults - which serve the interface on port 18883
     * and MQTT on 1883 and 8883, with persistence off.
     *
     * @param configurationFile Path to create.
     * @param fallbackTemplate  Template to use when there is none beside the file; the installed
     *                          one by default.
     * @return what the configuration was made from, or empty when the file was already there.
     */
    static sptk::String createConfiguration(const std::filesystem::path& configurationFile,
                                            const std::filesystem::path& fallbackTemplate = configurationTemplatePath());

    /**
     * @brief Replace the configuration with a starting one, keeping the old file beside it.
     *
     * The way back from a configuration that will not start - a port already taken, a listener
     * bound to an address this host does not have - when the interface that would fix it is the
     * very thing that is not coming up. Run from the command line, so it works with nothing
     * running at all.
     *
     * The old file is renamed rather than removed: it is usually still wanted, if only to see
     * what was wrong with it. Accounts are left alone; they are in a file of their own and are
     * not what stops a server from starting.
     *
     * @param configurationFile Configuration to replace.
     * @param fallbackTemplate  Template to use when there is none beside the file; the installed
     *                          one by default.
     * @return what was done, for reporting.
     */
    static sptk::String resetConfiguration(const std::filesystem::path& configurationFile,
                                           const std::filesystem::path& fallbackTemplate = configurationTemplatePath());

    /**
     * @brief Set an account's password from outside the configuration interface.
     *
     * The interface is normally where accounts are managed, but it cannot always be reached. A
     * server whose administrator has no password yet binds the interface to the loopback address
     * (see administratorPasswordSet()), which on a headless host means an SSH tunnel and inside a
     * container means nothing at all: the container's loopback is not the host's, so a published
     * port reaches no listener. That leaves the password needed to open the interface obtainable
     * only through the interface. This is the way out, and the way to install a server without a
     * person answering a page afterwards.
     *
     * Only the password changes; the account keeps its rights and everything else. Saving is what
     * the user manager already does when an account is modified.
     *
     * @param configurationFile Configuration to read, for where the accounts live.
     * @param usersFile         Users file, or empty for the one beside the configuration.
     * @param username          Account to change.
     * @param password          The new password, in plain text; it is stored as a token.
     * @return what was done, for reporting.
     * @throws sptk::Exception when there is no such account.
     */
    static sptk::String setAccountPassword(const std::filesystem::path& configurationFile,
                                           const std::filesystem::path& usersFile,
                                           const sptk::String& username,
                                           const sptk::String& password);

private:
    mutable sptk::ReadWriteMutex  m_mutex;                ///< Mutex that protects access to member variables.
    bool                          m_changed {false};      ///< True if the configuration was changed.
    LogSubjectPriorities          m_logSubjectPriorities; ///< Log subject priorities.
    std::filesystem::path         m_configurationPath;    ///< Path to the configuration file.
    std::filesystem::path         m_usersPath;            ///< Path to the users file, when given explicitly.
    BridgeManager                 m_bridgeManager;        ///< Bridge manager.
    ListenerManager               m_listenerManager;      ///< Listener manager.
    UserManager                   m_userManager;          ///< Authentication and users.
    /// Whether the accounts live in a database. Kept here rather than asked of the manager: the
    /// manager asks this while holding its own lock, and asking it back would deadlock on it.
    bool                          m_accountsInDatabase {false};

    /**
     * @brief Initialize internal data from the JSON object.
     */
    void initialize();

    /**
     * @brief Replace every account with the ones a freshly configured server has.
     *
     * The administrator, with the password given, and the account cluster peers connect as.
     * Anything else belonged to the configuration being replaced.
     *
     * @param administratorPassword Password the administrator will sign in with.
     */
    void resetUsersUnlocked(const sptk::String& administratorPassword);

    void loadConfigurationUnlocked(const sptk::Buffer& config);
    void saveConfigurationUnlocked(sptk::Buffer& config) const;
    void saveConfigurationUnlocked() const;
    void updateUsers();

    /**
     * @brief Path of the users file: the one given on the command line, or 'xmq_users.conf'
     * beside the configuration file it belongs to.
     * @return the users file path, or empty when the configuration was not loaded from a file.
     */
    [[nodiscard]] std::filesystem::path usersPathUnlocked() const;

public:
    /**
     * @brief Where the accounts are kept: the configured database, or an SQLite file.
     *
     * The setting names any database SPTK can reach. Left out, it is xmq_users.db beside the
     * configuration file - the same rule the users file follows, so a fresh installation needs no
     * database and no setting, and two brokers configured in different directories do not share
     * accounts without being told to.
     *
     * @return A connection string, or empty when the configuration came from no file and none was
     *         configured - in which case there is nowhere to put a default.
     */
    [[nodiscard]] sptk::String userDatabaseUri() const;

    /// Hashing cost for new passwords, as configured, or PasswordHash's own default.
    [[nodiscard]] int passwordIterations() const;

    /**
     * @brief The secret this node presents to the other nodes of its cluster.
     *
     * The same on every node, and readable here because a node has to be able to send it. What
     * incoming peers are checked against is the 'cluster' account's verifier in the accounts
     * database, which cannot be read back - presenting a secret and checking one are different
     * needs, and storing one thing for both is what kept every password recoverable.
     *
     * @return the configured secret, or empty when the node has never been clustered.
     */
    [[nodiscard]] sptk::String clusterPassword() const;

private:
    /**
     * @brief Takes the cluster secret out of the accounts and into the configuration.
     *
     * Called on the one reading that can still see it - before the accounts become verifiers -
     * because a node has to present this secret to its peers and a verifier cannot be presented.
     * Does nothing when the configuration already carries one.
     */
    void captureClusterSecretUnlocked();

    /**
     * @brief Opens the accounts database, migrates the file into it, and hands it to the manager.
     *
     * Failing to reach it is reported and no more: the accounts read from the file are already in
     * memory, so the broker goes on working with them rather than starting with nobody able to
     * sign in. What must not happen is the other reading - that an unreachable database means no
     * accounts and therefore no passwords - and it does not, because nothing is cleared here.
     */
    void openUserDatabaseUnlocked();

    /// Where the accounts live, for callers that already hold the lock.
    [[nodiscard]] sptk::String userDatabaseUriUnlocked() const;

    /// The configured hashing cost, for callers that already hold the lock.
    [[nodiscard]] int passwordIterationsUnlocked() const;

public:

private:

    /**
     * @brief Read the accounts from the users file into the authentication settings.
     *
     * Accounts kept in the configuration file are used when there is no users file yet, so a
     * configuration written by an older version still works.
     *
     * @return true if the users file existed; false when the accounts came from the configuration
     *         and the caller must write them out before the configuration is saved without them.
     */
    [[nodiscard]] bool loadUsersUnlocked();

    /**
     * @brief Write the accounts to the users file.
     *
     * Kept out of the configuration file deliberately: credentials belong in a file of their own,
     * which can be given its own permissions, and which stays readable when the configuration
     * beside it does not parse.
     */
    void saveUsersUnlocked() const;

    /// Writes the accounts to a named file, whatever the accounts' home is.
    void saveUsersToFileUnlocked(const std::filesystem::path& usersPath) const;
};

} // namespace xmq
