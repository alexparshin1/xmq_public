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

#include "UserManager.h"

#include "PasswordHash.h"
#include <ranges>
#include <set>
#include <sptk5/JWT.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
const String key256("Error: The encryption key is not defined");
}

UserManager::UserManager(OnChangeEvent onChange)
    : m_onChange(std::move(onChange))
{
}

UserManager::UserManager(const UserManager& other)
    : m_users(other.m_users)
    , m_passwords(other.m_passwords)
    , m_onChange(other.m_onChange)
{
}

void UserManager::loadConfiguration(CAuthentication& authentication, bool& changed)
{
    m_passwords.clear();
    m_users.clear();

    m_allowAnonymous = authentication.m_allow_anonymous;
    m_iterations = authentication.m_password_iterations.isNull()
                       ? PasswordHash::defaultIterations
                       : authentication.m_password_iterations.asInteger();

    setMissingIds(authentication.m_users, changed);

    for (auto& user: authentication.m_users)
    {
        if (user.m_is_enabled.isNull())
        {
            user.m_is_enabled = true;
        }

        // What is stored is a verifier, and what was stored before this version was a JWT with the
        // password inside it in clear text. An account still in the old form is converted here, on
        // the one reading that can still see the password - so an existing installation needs
        // nobody to choose a new password, and afterwards there is nothing left to convert.
        if (!PasswordHash::isHashed(user.m_password.asString()))
        {
            if (const auto legacy = legacyPassword(String(user.m_password.asString())))
            {
                // An empty one is not a password to convert: it is the account whose password has
                // never been set, which the first-start administrator is, and hashing it would
                // give that account a verifier for the empty string - so an empty password would
                // then let anyone in through the very door that state exists to hold open only
                // for whoever is at the machine.
                user.m_password = legacy->empty() ? String() : PasswordHash::hash(*legacy, m_iterations);
                changed = true;
            }
            else
            {
                // Neither a verifier nor a password anyone can read. Left as it is, which
                // authenticates nobody: replacing it would set a password nobody chose.
                CERR("The stored password of '" << user.m_username.asString()
                                                << "' cannot be read, so that account cannot sign in");
            }
        }

        m_users[user.m_id] = user;
        m_passwords[user.m_username] = {user.m_id, user.m_password.asString()};
    }

    m_verificationCache.sizeFor(m_passwords.size());
}

std::optional<String> UserManager::legacyPassword(const String& stored)
{
    if (stored.empty())
    {
        // An account whose password has never been set, which is a state of its own - see
        // authenticate() - and not something to convert.
        return {};
    }

    try
    {
        JWT jwt;
        jwt.set_alg(JWT::Algorithm::HS256, key256);
        jwt.decode(stored.c_str(), key256);
        return String(jwt.get("password").asString());
    }
    catch (const Exception&)
    {
        return {};
    }
}

bool UserManager::authenticate(const std::string& username, const std::string& password) const
{
    return authenticateWithOutcome(username, password) == AuthOutcome::Allowed;
}

UserManager::AuthOutcome UserManager::authenticateWithOutcome(const std::string& username,
                                                              const std::string& password) const
{
    if (username.empty())
    {
        const shared_lock lock(m_mutex);
        return m_allowAnonymous ? AuthOutcome::Allowed : AuthOutcome::AnonymousRefused;
    }

    // Asked before anything is locked. Verifying a password is 1.13 ms here at 10000 iterations -
    // 884 a second on one core - against thousands of connections a second on the CONNECT path, so
    // this is what makes a costed KDF affordable at all. Caching the *account* would not: the
    // record is in memory either way and the cost is the arithmetic.
    switch (m_verificationCache.look(username, password))
    {
        case VerificationCache::Answer::Allowed:
            return AuthOutcome::Allowed;
        case VerificationCache::Answer::Refused:
            // A remembered refusal is always a wrong password: a name that is not here is never
            // remembered, so it cannot be what this is.
            return AuthOutcome::BadCredentials;
        case VerificationCache::Answer::Unknown:
            break;
    }

    const auto verifier = [&]() -> std::string
    {
        const shared_lock lock(m_mutex);
        const auto        iterator = m_passwords.find(username);
        return iterator == m_passwords.end() ? std::string() : iterator->second.m_password;
    }();

    // An account with no password is one whose password has never been set, and nothing may be
    // authenticated against it - least of all an empty password, which is what a client that sent
    // no password at all arrives with. The freshly installed administrator is such an account,
    // and it is admitted by the configuration interface alone, which is bound to the loopback
    // address for exactly as long as that is true. A name that is not here reads the same way, and
    // neither is remembered: a map lookup is nanoseconds, and remembering names nobody has would
    // let anyone fill the table by inventing them.
    if (verifier.empty())
    {
        return AuthOutcome::NoAccount;
    }

    // Verified with the lock released. It was held across this before, so every connection stopped
    // every account edit for a millisecond, and stopped every other connection reading the map.
    const auto allowed = PasswordHash::verify(password, verifier);

    // Refusals too. Otherwise a client sending wrong passwords costs a full KDF each time, which is
    // a way to spend the broker's cores from outside; a corrected password is a different entry and
    // is verified properly, so this delays no repair.
    m_verificationCache.remember(username, password, allowed);
    return allowed ? AuthOutcome::Allowed : AuthOutcome::BadCredentials;
}

bool UserManager::hasPassword(const std::string& username) const
{
    const shared_lock lock(m_mutex);

    const auto iterator = m_passwords.find(username);
    return iterator != m_passwords.end() && !iterator->second.m_password.empty();
}

WSArray<CUser> UserManager::getUsers(const String& regexFilter, const bool scramblePasswords) const
{
    shared_ptr<RegularExpression> filter;
    if (!regexFilter.empty() && regexFilter != ".*")
    {
        filter = make_shared<RegularExpression>(regexFilter);
    }

    WSArray<CUser> users("users");

    for (const auto& user: views::values(m_users))
    {
        if (!filter || filter->matches(user.m_username.asString()))
        {
            if (scramblePasswords)
            {
                CUser userCopy(user);
                userCopy.m_password = "*****";
                users.push_back(userCopy);
            }
            else
            {
                users.push_back(user);
            }
        }
    }

    return users;
}

string UserManager::makeUserToken(const string_view username, const string_view password, const string_view department,
                                  const string_view company, const DateTime& expires)
{
    JWT          jwt;
    stringstream token;

    jwt.set("iat", static_cast<int>(time(nullptr)));
    jwt.set("iss", "settings");
    jwt.set("exp", expires);
    jwt.set("password", password.data());
    jwt.set_alg(JWT::Algorithm::HS256, key256);

    const auto& info = jwt.grants.root()->pushNode("info");
    info->set("username", username);
    info->set("department", department);
    info->set("company", company);

    jwt.encode(token);

    return token.str();
}

void UserManager::setUserUnlocked(const CUser& user)
{
    const String given = user.m_password;

    String verifier;
    if (m_passwords.contains(user.m_username.asString()) && given.starts_with("*") && given.ends_with("*"))
    {
        // The password came back masked, meaning "keep the one already stored". What is kept is
        // the verifier itself: there is no password here to hash again, and that is the point.
        verifier = m_passwords[user.m_username].m_password;
    }
    else if (PasswordHash::isHashed(given))
    {
        // Already a verifier - an account being written back as it was read.
        verifier = given;
    }
    else if (!given.empty())
    {
        // Judged here, where somebody has just chosen it. A password that arrives from elsewhere -
        // migrated from an older broker's accounts file, or generated for the cluster account -
        // never reaches this branch: it is already a verifier by then, and refusing it would break
        // an installation that worked yesterday.
        PasswordHash::refuseIfWeak(given);
        verifier = PasswordHash::hash(given, m_iterations > 0 ? m_iterations : PasswordHash::defaultIterations);
    }
    // An empty password stays empty, and is a state of its own: an account whose password has
    // never been set, which authenticate() admits nobody against. See the comment there.

    auto       modifiedUser = user;
    const auto userId = user.m_id.isNull() ? nextSerialId() : user.m_id.asInteger();
    modifiedUser.m_id.setInt64(userId);
    modifiedUser.m_is_enabled = user.m_is_enabled.isNull() ? true : user.m_is_enabled.asBool();
    modifiedUser.m_password = verifier;
    // Membership is not kept here. It lives in the database, where it can be asked about from
    // either end, and a copy in this record would be one that nothing keeps up to date.
    modifiedUser.m_groups.clear();
    m_passwords[user.m_username] = {userId, verifier};

    // Disabling the last one able to administer the broker leads where deleting it does, so it is
    // refused in the same way. Any other account, including the one named admin, may be disabled
    // while somebody else can still administer.
    if (!modifiedUser.m_is_enabled.asBool())
    {
        refuseIfLastAdministratorUnlocked(String(modifiedUser.m_username.asString()));
    }

    m_users[modifiedUser.m_id.asInt64()] = std::move(modifiedUser);
}

[[maybe_unused]] CUser UserManager::getUser(const uint64_t id)
{
    shared_lock lock(m_mutex);

    const auto iterator = m_users.find(id);
    if (iterator == m_users.end())
    {
        throw Exception("User doesn't exist: id=" + to_string(id));
    }

    return iterator->second;
}

CUser UserManager::findUser(const string& username)
{
    shared_lock lock(m_mutex);

    auto sameUsername = [&username](const CUser& user)
    {
        return user.m_username.asString() == username;
    };

    const auto iterator = ranges::find_if(views::values(m_users), sameUsername);
    if (iterator == views::values(m_users).end())
    {
        throw Exception("User doesn't exist: " + username);
    }

    return *iterator;
}

bool UserManager::hasUser(const string& username) const
{
    const shared_lock lock(m_mutex);

    return m_passwords.contains(username);
}

void UserManager::addUser(const CUser& user)
{
    unique_lock lock(m_mutex);

    if (m_passwords.contains(user.m_username.asString()))
    {
        throw Exception("User already exists: " + user.m_username.asString());
    }

    setUserUnlocked(user);
    storeUserUnlocked(m_users[m_passwords[user.m_username].m_id]);
    m_verificationCache.forget();
    m_onChange();
}

void UserManager::modifyUser(const CUser& user)
{
    unique_lock lock(m_mutex);

    auto userId = user.m_id.asInt64();
    if (userId == 0)
    {
        // User id is not defined, search for username instead
        if (const auto iterator = m_passwords.find(user.m_username.asString());
            iterator != m_passwords.end())
        {
            userId = static_cast<int64_t>(iterator->second.m_id);
        }
    }

    if (!m_users.contains(userId))
    {
        throw Exception("User doesn't exists: " + user.m_username.asString());
    }

    // The id found by name is put back into the record before it is stored. Without this, a
    // modification that arrived without one was looked up by name to check that the account
    // exists, and then saved under a freshly allocated id - leaving the account it was meant to
    // change beside the new one, and the list showing the same username twice.
    CUser modifiedUser(user);
    modifiedUser.m_id.setInt64(userId);

    setUserUnlocked(modifiedUser);
    storeUserUnlocked(m_users[static_cast<uint64_t>(userId)]);
    m_verificationCache.forget();
    m_onChange();
}

void UserManager::removeUser(const CUser& user)
{
    unique_lock lock(m_mutex);

    refuseIfLastAdministratorUnlocked(String(user.m_username.asString()));

    auto userId = user.m_id.asInt64();
    if (userId == 0)
    {
        const auto iterator = m_passwords.find(user.m_username.asString());
        if (iterator == m_passwords.end())
        {
            throw Exception("User doesn't exists: " + user.m_username.asString());
        }
        userId = static_cast<int64_t>(iterator->second.m_id);
    }
    if (!m_users.contains(userId))
    {
        throw Exception("User doesn't exists: id=" + user.m_username.asString());
    }

    m_passwords.erase(user.m_username);
    m_users.erase(user.m_id);

    if (m_store)
    {
        m_store->removeUser(userId);
    }

    m_verificationCache.forget();
    m_onChange();
}

void UserManager::allowAnonymous(const bool allow)
{
    unique_lock lock(m_mutex);

    m_allowAnonymous = allow;
    m_verificationCache.forget();
    m_onChange();
}

bool UserManager::isAllowAnonymous() const
{
    return m_allowAnonymous;
}

void UserManager::useStore(std::shared_ptr<UserStore> store)
{
    {
        const unique_lock lock(m_mutex);
        m_store = std::move(store);
    }
    reload();
}

bool UserManager::hasStore() const
{
    const shared_lock lock(m_mutex);
    return m_store != nullptr;
}

void UserManager::reload()
{
    const auto store = [this]
    {
        const shared_lock lock(m_mutex);
        return m_store;
    }();

    if (!store)
    {
        return;
    }

    // Read outside the lock: this is a database, and holding the lock that authenticate() needs
    // across a network round trip would stop every connection for the length of it.
    const auto stored = store->users();

    std::map<uint64_t, CUser>                    users;
    std::map<std::string, UserInfo, std::less<>> passwords;
    for (const auto& user: stored)
    {
        CUser entry;
        entry.m_id.setInt64(user.m_id);
        entry.m_username = user.m_username;
        entry.m_password = user.m_password;
        entry.m_is_enabled = user.m_enabled;

        users[static_cast<uint64_t>(user.m_id)] = entry;
        passwords[user.m_username] = {static_cast<uint64_t>(user.m_id), user.m_password};
    }

    const unique_lock lock(m_mutex);
    m_users = std::move(users);
    m_passwords = std::move(passwords);
    m_verificationCache.sizeFor(m_passwords.size());
}

void UserManager::storeUserUnlocked(const CUser& user) const
{
    if (!m_store)
    {
        return;
    }

    const UserStore::User record {.m_id = user.m_id.asInt64(),
                                  .m_username = user.m_username.asString(),
                                  .m_password = user.m_password.asString(),
                                  .m_enabled = user.m_is_enabled.asBool(),
                                  .m_updatedBy = "interface"};

    if (m_store->findUser(record.m_username))
    {
        m_store->updateUser(record);
    }
    else
    {
        m_store->addUser(record);
    }
}

void UserManager::replaceStoredAccounts()
{
    const unique_lock lock(m_mutex);

    if (!m_store)
    {
        return;
    }

    std::set<String> wanted;
    for (const auto& user: views::values(m_users))
    {
        wanted.insert(String(user.m_username.asString()));
        storeUserUnlocked(user);
    }

    for (const auto& stored: m_store->users())
    {
        if (!wanted.contains(stored.m_username))
        {
            m_store->removeUser(stored.m_id);
        }
    }
}

namespace {

/// The database or nothing: groups exist only there, and pretending otherwise would let the
/// screen appear to work on a broker whose accounts are still in a file.
const char* noStoreMessage = "Groups need the accounts to be kept in a database";

} // namespace

void UserManager::addGroup(const String& name)
{
    const unique_lock lock(m_mutex);

    if (!m_store)
    {
        throw Exception(noStoreMessage);
    }
    m_store->addGroup({.m_name = name, .m_updatedBy = "interface"});
}

void UserManager::removeGroup(const String& name)
{
    const unique_lock lock(m_mutex);

    if (!m_store)
    {
        throw Exception(noStoreMessage);
    }

    const auto group = m_store->findGroup(name);
    if (!group)
    {
        throw Exception("Group doesn't exist: " + name);
    }

    if (name == String(string(UserStore::administratorsGroupName)) &&
        !enabledAdministratorsUnlocked().empty())
    {
        // Removing the group empties it, which is every administrator at once - the same end as
        // the other three ways of getting there, reached in one step.
        throw Exception("The Administrators group cannot be removed while anybody is in it: "
                        "the broker would be left with nobody able to administer it");
    }

    m_store->removeGroup(group->m_id);
}

WSArray<CUserGroup> UserManager::getGroups() const
{
    const shared_lock lock(m_mutex);

    WSArray<CUserGroup> groups("list");
    if (!m_store)
    {
        return groups;
    }

    for (const auto& group: m_store->groups())
    {
        CUserGroup entry;
        entry.m_id.setInt64(group.m_id);
        entry.m_name = group.m_name;
        groups.push_back(entry);
    }
    return groups;
}

bool UserManager::isAdministrator(const std::string& username) const
{
    const shared_lock lock(m_mutex);

    if (!m_store)
    {
        // No database, so no groups: the accounts are still in a file, and the only account that
        // could administer anything there is the one the setup path admits.
        return username == "admin";
    }

    const auto user = m_store->findUser(username);
    if (!user)
    {
        return false;
    }

    for (const auto& group: m_store->groupsOfUser(user->m_id))
    {
        if (group.m_name == String(string(UserStore::administratorsGroupName)))
        {
            return true;
        }
    }
    return false;
}

Strings UserManager::groupsOf(const std::string& username) const
{
    const shared_lock lock(m_mutex);

    Strings names;
    if (!m_store)
    {
        return names;
    }

    const auto user = m_store->findUser(username);
    if (!user)
    {
        return names;
    }

    for (const auto& group: m_store->groupsOfUser(user->m_id))
    {
        names.push_back(group.m_name);
    }
    return names;
}

void UserManager::setGroupsOf(const std::string& username, const Strings& groupNames)
{
    const unique_lock lock(m_mutex);

    if (!m_store)
    {
        throw Exception(noStoreMessage);
    }

    const auto wantedNames = groupNames;
    if (ranges::find(wantedNames, String(string(UserStore::administratorsGroupName))) == wantedNames.end())
    {
        // On its way out of Administrators, which for the last enabled one of them is the same as
        // taking administration away from everybody.
        refuseIfLastAdministratorUnlocked(String(username));
    }

    const auto user = m_store->findUser(username);
    if (!user)
    {
        throw Exception("User doesn't exist: " + username);
    }

    // Every name is looked up before anything is changed, so that a list with one bad name in it
    // leaves the membership as it was rather than half applied.
    std::map<String, int64_t> wanted;
    for (const auto& name: wantedNames)
    {
        if (name.empty())
        {
            continue;
        }
        const auto group = m_store->findGroup(name);
        if (!group)
        {
            throw Exception("Group doesn't exist: " + name);
        }
        wanted[name] = group->m_id;
    }

    for (const auto& group: m_store->groupsOfUser(user->m_id))
    {
        if (!wanted.contains(group.m_name))
        {
            m_store->removeUserFromGroup(user->m_id, group.m_id);
        }
    }

    for (const auto& [name, id]: wanted)
    {
        m_store->addUserToGroup(user->m_id, id);
    }
}

std::vector<String> UserManager::enabledAdministratorsUnlocked() const
{
    std::vector<String> administrators;
    if (!m_store)
    {
        return administrators;
    }

    const auto group = m_store->findGroup(String(string(UserStore::administratorsGroupName)));
    if (!group)
    {
        return administrators;
    }

    for (const auto& member: m_store->usersInGroup(group->m_id))
    {
        if (member.m_enabled)
        {
            administrators.push_back(member.m_username);
        }
    }
    return administrators;
}

void UserManager::refuseIfLastAdministratorUnlocked(const String& username) const
{
    if (!m_store)
    {
        return;
    }

    const auto administrators = enabledAdministratorsUnlocked();
    if (administrators.size() != 1 || administrators.front() != username)
    {
        return;
    }

    throw Exception("'" + username + "' is the only enabled administrator, and a broker nobody "
                                     "can administer cannot be undone from here. Give somebody else "
                                     "administrative rights first.");
}
