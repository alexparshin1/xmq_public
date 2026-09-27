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

#include "SerialId.h"
#include "UserStore.h"
#include "VerificationCache.h"
#include "service/CAuthentication.h"
#include "service/CUserGroup.h"
#include <optional>
#include <shared_mutex>
#include <sptk5/cutils>

namespace xmq {

class UserManager final : public SerialId
{
public:
    using OnChangeEvent = std::function<void()>;

    /**
     * @brief Constructor.
     */
    explicit UserManager(OnChangeEvent onChange);

    /**
     * @brief Copy constructor.
     */
    UserManager(const UserManager& other);

    /**
     * @brief Destructor.
     */
    ~UserManager() = default;

    void loadConfiguration(CAuthentication& authentication, bool& changed);

    /**
     * @brief Takes the accounts from a database instead of a file, and keeps them there.
     *
     * The in-memory maps stay: authenticate() runs on the CONNECT path and a database round trip
     * there would be paid on every connection. They are filled from the database and refilled when
     * it changes - which is the one place a change is applied, so that a change that will not
     * apply fails the same way everywhere rather than on some nodes only.
     *
     * @param store         Where the accounts live, or nullptr to go back to the file.
     */
    void useStore(std::shared_ptr<UserStore> store);

    /// Reads every account and its groups out of the database into memory.
    void reload();

    /**
     * @brief Makes the database hold exactly the accounts held in memory, and nothing else.
     *
     * For the setup screen, which replaces the accounts rather than editing them: without this the
     * database would keep whoever was there before, and since it is what a restart reads, the
     * accounts just configured would last only until then.
     */
    void replaceStoredAccounts();

    /// Whether the accounts come from a database rather than from the file.
    [[nodiscard]] bool hasStore() const;

    /// Why authentication answered as it did. Finer than what the client is told, deliberately:
    /// a client learns only that it failed, while an extension counting failures needs to tell a
    /// sweep of invented names from a password being guessed at one account.
    enum class AuthOutcome
    {
        Allowed,
        AnonymousRefused, ///< No username, and anonymous access is not allowed.
        NoAccount,        ///< No such account, or one whose password has never been set.
        BadCredentials    ///< The account exists and the password is not its own.
    };

    [[nodiscard]] AuthOutcome authenticateWithOutcome(const std::string& username,
                                                      const std::string& password) const;

    bool authenticate(const std::string& username, const std::string& password) const;

    static std::string makeUserToken(std::string_view username, std::string_view password, std::string_view department,
                                     std::string_view company, const sptk::DateTime& expires);

    sptk::WSArray<CUser>      getUsers(const sptk::String& regexFilter, bool scramblePasswords) const;
    [[nodiscard]] CUser       getUser(uint64_t id);
    CUser                     findUser(const std::string& username);

    /**
     * @brief Whether an account of this name exists.
     *
     * Asked instead of findUser() when the answer decides between adding and modifying: findUser()
     * reports an absent account by throwing, which is not news at that point.
     */
    [[nodiscard]] bool hasUser(const std::string& username) const;

    /**
     * @brief Whether an account of this name has a password set.
     *
     * An account with none cannot be authenticated at all - see authenticate(). Asked by the
     * configuration interface about the administrator, because a freshly installed server has
     * one and has to say so rather than simply refusing to let anyone in.
     */
    [[nodiscard]] bool hasPassword(const std::string& username) const;

    void                      addUser(const CUser& user);
    void                      modifyUser(const CUser& user);
    void                      removeUser(const CUser& user);
    /**
     * @brief Creates a group.
     * @throws sptk::Exception when the accounts are not in a database, or the name is taken.
     */
    void addGroup(const sptk::String& name);

    /// Removes a group and everybody's membership of it. The accounts themselves stay.
    void removeGroup(const sptk::String& name);

    /// Every group there is, by name.
    [[nodiscard]] sptk::WSArray<CUserGroup> getGroups() const;

    /**
     * @brief Whether this account may administer the broker.
     * @remarks Membership of the Administrators group, and nothing else. It used to be a field on
     *          the account as well, which is two places answering one question - and the two could
     *          disagree.
     */
    [[nodiscard]] bool isAdministrator(const std::string& username) const;

    /// The groups one account belongs to, by name.
    [[nodiscard]] sptk::Strings groupsOf(const std::string& username) const;

    /**
     * @brief Makes an account belong to exactly these groups and no others.
     * @remarks Sent whole by the screen on every save, so this has to be a statement of what the
     *          membership should be rather than a change to it - the caller does not know which
     *          of them were already there.
     * @throws sptk::Exception naming a group that does not exist, rather than creating it: a
     *         mistyped name would otherwise quietly become a group of its own.
     */
    void setGroupsOf(const std::string& username, const sptk::Strings& groupNames);

    void                      allowAnonymous(bool allow);
    bool                      isAllowAnonymous() const;

private:
    struct UserInfo
    {
        uint64_t    m_id;
        std::string m_password;
    };

    /**
     * @brief What verifying a password answered last time, so the KDF is not run again.
     *
     * Mutable because authenticate() is const and has to record what it found: the cache is part of
     * how the question is answered, not part of what the answer says.
     */
    mutable VerificationCache m_verificationCache;

    mutable std::shared_mutex                    m_mutex;
    std::map<uint64_t, CUser>                    m_users;     ///< User index
    std::map<std::string, UserInfo, std::less<>> m_passwords; ///< User passwords
    OnChangeEvent                                m_onChange;  ///< Event called when users are changed
    bool                                         m_allowAnonymous {false};
    int                                          m_iterations {0}; ///< Hashing cost for new passwords.
    std::shared_ptr<UserStore>                   m_store;          ///< Where the accounts live, when not the file.

    void setUserUnlocked(const CUser& user);

    /// Writes one account to the database, when that is where the accounts live.
    void storeUserUnlocked(const CUser& user) const;

    /**
     * @brief The accounts that are both enabled and in the Administrators group.
     * @remarks Called with the lock already held.
     */
    [[nodiscard]] std::vector<sptk::String> enabledAdministratorsUnlocked() const;

    /**
     * @brief Refuses a change that would leave nobody able to administer the broker.
     *
     * Deleting an account, disabling it, taking it out of Administrators and removing the group
     * itself all lead to the same place, so they all ask this. The rule is about the last one
     * standing rather than about any particular account: while somebody else can administer the
     * broker, every account - including the one named admin - can be removed, disabled or moved.
     *
     * @param username  The account about to be removed, disabled or taken out of the group.
     * @throws sptk::Exception when that account is the last enabled administrator.
     */
    void refuseIfLastAdministratorUnlocked(const sptk::String& username) const;

public:
    /**
     * @brief The password inside a stored value from before verifiers, if that is what it is.
     * @remarks The old form is a JWT whose payload holds the password in clear text. Reading it is
     *          how an existing installation converts without asking anyone to choose a new one.
     * @return The password, or nothing when the value is not one of those.
     */
    static std::optional<sptk::String> legacyPassword(const sptk::String& stored);

private:
};

} // namespace xmq
