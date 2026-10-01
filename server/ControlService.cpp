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

#include "ControlService.h"

#include "SelfSignedCertificate.h"
#include "ServerController.h"
#include "storage/RedisStorage.h"

#include <sptk5/net/RedisCommand.h>
#include <sptk5/net/RedisConnect.h>

#include <array>
#include <algorithm>
#include <climits>
#include <ctime>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "common/DirectoryNames.h"

#include <ranges>

using namespace std;
using namespace sptk;
using namespace xmq;

ControlService::ControlService(IServerController* controller)
    : m_controller(controller)
{
}

Server* ControlService::server() const
{
    return m_controller->getServer();
}

const shared_ptr<Settings>& ControlService::getSettings() const
{
    return m_controller->getSettings();
}

Server& ControlService::requireServer() const
{
    const auto runningServer = server();
    if (!runningServer)
    {
        throw Exception("The server is stopped. Start it to use this operation.");
    }
    return *runningServer;
}

void ControlService::requireReachableRedis(const CInitialSetup& setup)
{
    const auto redisHost = String(setup.m_redis_host.asString()).trim();
    if (redisHost.empty())
    {
        // No address means no persistence, which is a working configuration and nothing to check.
        return;
    }

    const auto redisPort = setup.m_redis_port.asInteger();
    constexpr auto highestPort = 65535;
    if (redisPort < 1 || redisPort > highestPort)
    {
        // Left to applyInitialSetup, which refuses it with the rest of the port checking. There
        // is nothing here to connect to in the meantime.
        return;
    }

    // Really connected to, not merely reached: RedisConnect speaks the protocol and comes back
    // with the server's own description, so something else listening on the port is caught too.
    //
    // Asked before anything is written. A setup naming a Redis that is not there produces a
    // broker which starts, quietly falls back to keeping everything in memory, and mentions it
    // only in the log - which is not where the person who just pressed Submit is looking.
    try
    {
        RedisConnect probe;
        (void) probe.connect(redisHost.c_str(), static_cast<uint16_t>(redisPort));
        probe.disconnect();
    }
    catch (const Exception& exception)
    {
        // The reason as it came, without a full stop of our own: SPTK's messages already end
        // with one, and two in a row is what the reader notices instead of the reason.
        throw Exception("Redis at " + redisHost + ":" + to_string(redisPort) + " cannot be reached: " +
                        String(exception.message()).trim() + " Correct the address, or leave it "
                        "empty to run without persistence.");
    }
}

bool ControlService::isUnconfiguredAdministrator(const String& username, const String& password) const
{
    return password.empty() && username == Settings::administratorUsername() &&
           !getSettings()->administratorPasswordSet();
}

void ControlService::authenticate(HttpAuthentication* authentication, CUser& /*user*/)
{
    if (authentication && authentication->type() == HttpAuthentication::Type::BEARER)
    {
        constexpr auto jwtOffset = 7;
        if (const String jwt = authentication->getHeader().substr(jwtOffset);
            !m_jwtManager.validateToken(jwt))
        {
            throw Exception("Not authenticated or session expired");
        }

        const auto userData = authentication->getData()->findFirst("user");
        if (userData == nullptr)
        {
            throw Exception("Not authenticated: invalid JWT data");
        }
        const auto username = userData->getString("username");
        const auto password = userData->getString("password");
        if (isUnconfiguredAdministrator(username, password) ||
            getSettings()->userManager().authenticate(username, password))
        {
            return;
        }
    }
    throw Exception("Not authenticated");
}

void ControlService::GetClientSessions(const CGetClientSessions& input, CGetClientSessionsResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const std::string prefix = input.m_prefix.asString();
        const auto limit = input.m_limit.asInteger();
        if (prefix.size() > 256)
        {
            throw Exception("Client ID prefix is too long (maximum 256 characters).");
        }
        if (limit < 1 || limit > 500)
        {
            throw Exception("Session limit must be between 1 and 500.");
        }

        const auto sessions = requireServer().getClientSessionManager()->findSessions(prefix, limit);
        output.m_has_more = sessions.size() > static_cast<size_t>(limit);
        for (size_t index = 0; index < std::min(sessions.size(), static_cast<size_t>(limit)); ++index)
        {
            const auto& session = sessions[index];
            CClientSessionInfo info;
            info.m_client_id = session->getClientId();
            info.m_online = session->isConnected();
            info.m_persistent = !session->isCleanSession();
            info.m_subscription_count = static_cast<int>(std::min(session->subscriptionCount(), static_cast<size_t>(INT_MAX)));
            const auto queue = session->getInflightQueue();
            info.m_queued_messages = queue ? static_cast<int>(std::min(queue->size(), static_cast<size_t>(INT_MAX))) : 0;

            if (const auto connectedAt = session->connectedAt(); connectedAt != 0)
            {
                std::tm utc {};
#ifdef _WIN32
                gmtime_s(&utc, &connectedAt);
#else
                gmtime_r(&connectedAt, &utc);
#endif
                char timestamp[32] {};
                std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
                info.m_connected_at = timestamp;
            }
            output.m_client_sessions.push_back(std::move(info));
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::Login(const CLogin& input, CLoginResponse& output, HttpAuthentication*)
{
    try
    {
        const auto username = input.m_username.asString();
        const auto password = input.m_password.asString();

        if (!isUnconfiguredAdministrator(username, password) &&
            !getSettings()->userManager().authenticate(username, password))
        {
            throw Exception("Invalid username or password");
        }

        const auto user = getSettings()->userManager().findUser(username);

        if (!user.m_is_enabled.asBool())
        {
            throw Exception("User access is disabled");
        }

        if (!getSettings()->userManager().isAdministrator(username))
        {
            throw Exception("User doesn't have administrative privileges");
        }

        CUser tempUser;
        tempUser.m_username = username;
        tempUser.m_password = password;
        output.m_token = m_jwtManager.generateToken(tempUser);

        // Told at sign-in rather than left for a page to discover: it is what decides where the
        // browser goes next, and a server nobody has set up has one thing to do before anything
        // else on it means much.
        output.m_setup_required = !getSettings()->administratorPasswordSet();
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_description = e.message();
        output.m_result.m_success = false;
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::LoggingControl(const CLoggingControl& input, CLoggingControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto settings = getSettings();

        CLogging currentLogging;
        settings->loggingControl("get", currentLogging);

        if (input.m_action.asString() == "set")
        {
            CLogging logging(input.m_logging);
            settings->loggingControl(input.m_action, logging);
            try
            {
                if (const auto runningServer = server())
                {
                    runningServer->loggerReset();
                }
            }
            catch (const std::exception& e)
            {
                throw Exception(e.what());
            }
            catch (...)
            {
                throw Exception("Unknown error while resetting logger");
            }
        }
        else if (input.m_action.asString() == "get")
        {
            output.m_logging = currentLogging;
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::GetSubscriptions(const CGetSubscriptions&, CGetSubscriptionsResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        if (const auto runningServer = server())
        {
            for (auto        filteredSubscriptions = runningServer->getSubscriptionManager()->getSubscriptions("#");
                 const auto& topic: views::keys(filteredSubscriptions))
            {
                CSubscription subscription;
                subscription.m_topic = topic->fullName();
                output.m_subscription.push_back(subscription);
            }
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::SetupState(const CSetupState& /*input*/, CSetupStateResponse& output,
                                HttpAuthentication* /*authentication*/)
{
    // No authenticate() call, and that is the point: this is asked before anyone has signed in,
    // by a page deciding whether to offer a sign-in form at all. On a server that has never been
    // set up there is nobody to sign in as - the administrator has no password - so offering the
    // form means explaining an empty password to whoever installed it.
    try
    {
        output.m_setup_required = !getSettings()->administratorPasswordSet();
        output.m_result.m_success = true;
    }
    catch (const std::exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::applyGroups(const CUser& user) const
{
    // Only when the account came with a membership. An older client, or a screen that does not
    // know about groups, sends none - and that must leave the membership alone rather than read
    // as "belongs to nothing", which would quietly empty it.
    if (user.m_groups.empty())
    {
        return;
    }

    Strings names;
    for (const auto& group: user.m_groups)
    {
        names.push_back(String(group.asString()));
    }
    getSettings()->userManager().setGroupsOf(user.m_username.asString(), names);
}

namespace {

/// The declared type, as a name the interface can switch on. A secret is named so that the screen
/// masks it and never puts it in a field that a browser might remember.
const char* settingTypeName(const xmq_setting_type type)
{
    switch (type)
    {
        case XMQ_SETTING_INTEGER:
            return "integer";
        case XMQ_SETTING_BOOLEAN:
            return "boolean";
        case XMQ_SETTING_CHOICE:
            return "choice";
        case XMQ_SETTING_SECRET:
            return "secret";
        case XMQ_SETTING_STRING:
            break;
    }
    return "string";
}

void describeInto(const ExtensionHost::Description& from, CExtension& into)
{
    into.m_name = from.m_name;
    into.m_version = from.m_version;
    into.m_description = from.m_description;
    into.m_library = from.m_library.string();
    into.m_source = from.m_source.string();
    into.m_running = from.m_running;
    into.m_required = from.m_required;
    into.m_observer = (from.m_capabilities & XMQ_CAP_OBSERVER) != 0;
    into.m_authenticator = (from.m_capabilities & XMQ_CAP_AUTHENTICATOR) != 0;
    into.m_authorizer = (from.m_capabilities & XMQ_CAP_AUTHORIZER) != 0;
    into.m_events_delivered = static_cast<int>(from.m_eventsDelivered);
    into.m_admitted = static_cast<int>(from.m_admitted);
    into.m_refused = static_cast<int>(from.m_refused);
    into.m_store_errors = static_cast<int>(from.m_storeErrors);

    for (const auto& setting: from.m_settings)
    {
        CExtensionSetting described;
        described.m_name = setting.m_name;
        described.m_label = setting.m_label;
        described.m_description = setting.m_description;
        described.m_type = settingTypeName(setting.m_type);
        described.m_default_value = setting.m_defaultValue;
        described.m_choices = setting.m_choices;
        described.m_required = setting.m_required;
        described.m_declared = setting.m_declared;

        // A secret never leaves the broker. The screen shows that one is set, not what it is - a
        // connection URI with a password in it is the ordinary case, and a value that reaches the
        // browser reaches screenshots and bug reports with it.
        described.m_value = setting.m_type == XMQ_SETTING_SECRET
                                ? String(setting.m_value.empty() ? "" : string(ExtensionHost::secretMask))
                                : String(setting.m_value);

        into.m_settings.push_back(described);
    }
}

} // namespace

void ControlService::ExtensionControl(const CExtensionControl& input, CExtensionControlResponse& output,
                                      HttpAuthentication* authentication)
{
    try
    {
        ExtensionHost::Report report;

        CUser caller;
        // authenticate() already refuses anyone who is not an administrator.
        authenticate(authentication, caller);

        if (const auto action = input.m_action.asString();
            action == "enable" || action == "disable")
        {
            const auto name = input.m_name.asString();
            if (name.empty())
            {
                throw Exception("Which extension? The action needs a name");
            }
            report = m_controller->switchExtension(name.c_str(), action == "enable");
        }
        else if (action == "set")
        {
            const auto name = input.m_name.asString();
            if (name.empty())
            {
                throw Exception("Which extension? The action needs a name");
            }

            map<string, string> settings;
            for (const auto& setting: input.m_settings)
            {
                if (const auto key = setting.m_name.asString(); !key.empty())
                {
                    settings[key.c_str()] = setting.m_value.asString().c_str();
                }
            }
            report = m_controller->setExtensionSettings(name.c_str(), settings);
        }
        else if (action != "list")
        {
            throw Exception("Unknown action: " + action);
        }

        // Listed after acting, so the answer shows the state the action left behind rather than the
        // one it found.
        for (const auto& described: m_controller->describeExtensions())
        {
            CExtension one;
            describeInto(described, one);
            output.m_list.push_back(one);
        }

        // An action that refused is not a success with an explanation attached: it goes back as a
        // failure, so the screen shows it the way it shows every other failure. The list above is
        // still sent, because what the operator most needs after a refusal is the state as it now
        // stands - which is the state the refusal preserved.
        if (report.failed())
        {
            output.m_result.m_success = false;
            output.m_result.m_description = report.m_problems.join("\n");
            return;
        }

        output.m_message = report.m_notes.empty() ? String("Changes are accepted")
                                                  : report.m_notes.join("\n");
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::UserGroupControl(const CUserGroupControl& input, CUserGroupControlResponse& output,
                                      HttpAuthentication* authentication)
{
    try
    {
        CUser caller;
        authenticate(authentication, caller);

        auto& groups = getSettings()->userManager();
        const auto action = input.m_action.asString();

        if (action == "add")
        {
            groups.addGroup(String(input.m_group.m_name.asString()));
        }
        else if (action == "remove")
        {
            // The memberships go with it, by the cascade the schema declares. The accounts stay:
            // removing a group is about who may do what, not about who exists.
            groups.removeGroup(String(input.m_group.m_name.asString()));
        }
        else if (action == "list")
        {
            output.m_list = groups.getGroups();
        }

        // Group membership decides what an account may do, so the same reasoning as for accounts
        // themselves: an extension that cached the answer has to be told the question changed.
        if (action != "list")
        {
            if (const auto runningServer = server())
            {
                runningServer->extensions().accountsChanged();
            }
        }

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop, as elsewhere here: anything that is not an sptk::Exception would otherwise
        // escape and leave the caller waiting for a response that never comes.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::UserControl(const CUserControl& input, CUserControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto settings = getSettings();
        if (input.m_action.asString() == "add")
        {
            if (!input.m_user.isNull())
            {
                settings->userManager().addUser(input.m_user);
                applyGroups(input.m_user);
            }

            if (!input.m_allow_anonymous.isNull())
            {
                settings->userManager().allowAnonymous(input.m_allow_anonymous);
            }
        }
        else if (input.m_action.asString() == "modify")
        {
            if (!input.m_user.isNull())
            {
                settings->userManager().modifyUser(input.m_user);
                applyGroups(input.m_user);
            }

            if (!input.m_allow_anonymous.isNull())
            {
                settings->userManager().allowAnonymous(input.m_allow_anonymous);
            }
        }
        else if (input.m_action.asString() == "remove")
        {
            settings->userManager().removeUser(input.m_user);
        }
        else if (input.m_action.asString() == "list")
        {
            output.m_list = getSettings()->userManager().getUsers(input.m_filter, true);

            // Gathered before anything is put in the answer. Filling the answer as they are
            // collected leaves it half-built when one of them fails - accounts present, their
            // groups not - and a caller that does not check whether the call succeeded then shows
            // accounts that look as though they belong to nothing.
            for (auto& listed: output.m_list)
            {
                // The screen shows an account and its groups together and sends them back the same
                // way, so the list has to carry them. Filled from the database and not added to:
                // the database is what the membership is, and whatever the record happens to carry
                // is only what somebody last sent.
                listed.m_groups.clear();
                for (const auto& group: getSettings()->userManager().groupsOf(listed.m_username.asString()))
                {
                    WSString name;
                    name = group;
                    listed.m_groups.push_back(name);
                }
            }
            output.m_allow_anonymous = getSettings()->userManager().isAllowAnonymous();
        }

        // Whoever authenticates against these accounts is holding answers that have just stopped
        // being true. Told here, on the path that changed them: an extension caches a verified
        // password because the KDF cannot be paid per connection, and without this a revoked
        // account goes on connecting for as long as that cache lives.
        if (input.m_action.asString() != "list")
        {
            if (const auto runningServer = server())
            {
                runningServer->extensions().accountsChanged();
            }
        }

        // A password given to the administrator here ends the same state the setup page ends:
        // the interface stops answering on the loopback address alone. Asked for from this page
        // too, because a server that has never been set up can be given its password from
        // either. Nothing changes when the address does not, and the controller logs a rebind
        // it could not do - which is why the answer is not examined here: the accounts were
        // changed either way, and that is what this call reports on.
        if (input.m_action.asString() != "list")
        {
            string reason;
            m_controller->moveControlService(
                static_cast<uint16_t>(settings->m_web_service.m_listener_port.asInteger()), reason);
        }

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::ListenerControl(const CListenerControl& input, CListenerControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto   action = input.m_action.asString();
        const auto   settings = getSettings();
        const auto&  listener = input.m_listener;
        const auto   port = static_cast<uint16_t>(listener.m_port.asInteger());
        const string bindIp = listener.m_bind_ip.asString().empty() ? string("0.0.0.0") : listener.m_bind_ip.asString().c_str();

        // The configuration is written whether MQTT is running. Only opening and closing
        // the port needs a server, and a stopped one opens whatever it finds configured when it
        // is started.
        const auto runningServer = server();

        if (action == "add" || action == "modify")
        {
            settings->listenerControl(action, input.m_listener);
            if (runningServer)
            {
                const auto connectionType = listener.m_protocol.asString().toUpperCase() == "MQTT" ? ServerConnection::Type::TCP : ServerConnection::Type::SSL;
                runningServer->removeListener({bindIp, port});
                // Disabling a listener takes effect at once: the port is dropped and not reopened.
                if (listener.m_enable.isNull() || listener.m_enable.asBool())
                {
                    runningServer->addListener(connectionType, {bindIp, port}, static_cast<uint16_t>(listener.m_threads.asInteger()));
                }
            }
        }
        else if (action == "remove")
        {
            settings->listenerControl(action, input.m_listener);
            if (runningServer)
            {
                runningServer->removeListener({bindIp, port});
            }
        }
        else if (action == "list")
        {
            output.m_list = getSettings()->m_connections.m_listener;
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::PersistenceControl(const CPersistenceControl& input, CPersistenceControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);
        output.m_persistence = getSettings()->persistenceControl(input.m_action, input.m_persistence);
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

namespace {
/**
 * @brief Move a file out of the way, keeping it beside its replacement.
 *
 * Nothing installed here is ever deleted outright: the file being replaced may be the only copy
 * of a certificate somebody paid for, and the mistake is usually noticed after the fact.
 *
 * @param file                  File about to be replaced.
 */
void keepPrevious(const filesystem::path& file)
{
    if (!filesystem::exists(file))
    {
        return;
    }

    auto backupPath = file;
    backupPath += ".old";
    filesystem::rename(file, backupPath);
}

/**
 * @brief Install one uploaded certificate or key, and report where it landed.
 *
 * Empty content means the interface didn't upload that file, so the installed one is kept and
 * its existing path returned. Removing a file is not something an untouched form field should do.
 *
 * @param certificatesDirectory Directory the certificates live in.
 * @param filename              File name to write.
 * @param content               File content, or empty to keep what is installed.
 * @param currentPath           Path currently recorded in the configuration.
 * @param isPrivateKey          True for a private key, which is left readable by its owner only.
 * @return path of the file to record in the configuration.
 */
string writeCertificate(const filesystem::path& certificatesDirectory, const string& filename,
                        const string&           content, const string&               currentPath, const bool isPrivateKey = false)
{
    const Buffer data(content);
    if (data.empty())
    {
        return currentPath;
    }

    const auto fullPath = certificatesDirectory / filename;
    keepPrevious(fullPath);
    data.saveToFile(fullPath);

    if (isPrivateKey)
    {
        // Done after writing rather than by creating the file with the right mode, because the
        // umask a service inherits is not something to rely on. A private key other accounts on
        // the machine can read is a private key in name only.
        error_code errorCode;
        filesystem::permissions(fullPath, filesystem::perms::owner_read | filesystem::perms::owner_write,
                                filesystem::perm_options::replace, errorCode);
    }

    return fullPath.string();
}

/**
 * @brief The directory certificates are installed into, created if it is not there yet.
 * @return the directory.
 * @throws sptk::Exception when it cannot be created.
 */
filesystem::path certificatesDirectory()
{
    const auto directory(DirectoryNames::certsDirectory());
    if (!filesystem::exists(directory) && !filesystem::create_directories(directory))
    {
        throw Exception("Unable to create directory " + directory.string());
    }
    return directory;
}

/**
 * @brief Names the interface's own certificate is installed under.
 *
 * Its own names, rather than wherever certfile and keyfile happen to point: those may have been
 * aimed at the broker's pair, or at a certificate installed by hand and shared with something
 * else, and installing here must not write over either of them.
 *
 * @return certificate path and private key path.
 */
pair<filesystem::path, filesystem::path> webServiceKeyFiles()
{
    const auto directory = certificatesDirectory();
    return {directory / "webface.crt", directory / "webface.key"};
}

/**
 * @brief Report the certificate the interface is configured with.
 * @param settings              Server settings.
 * @param keys                  Receives the paths and what the certificate says about itself.
 */
void reportWebServiceKeys(const Settings& settings, CWebServiceKeys& keys)
{
    const auto certificateFile = settings.m_web_service.m_certfile.asString();
    keys.m_certfile = certificateFile;
    keys.m_keyfile = settings.m_web_service.m_keyfile.asString();

    // Read from the file every time rather than remembered: the interesting case is a certificate
    // replaced outside this interface, and a stored copy would describe the one that used to be
    // there.
    keys.m_description = SelfSignedCertificate::describe(filesystem::path(certificateFile.c_str()));
}

/**
 * @brief Record where the interface's certificate now lives.
 * @param settings              Server settings.
 * @param certificateFile       Installed certificate.
 * @param privateKeyFile        Installed private key.
 */
void recordWebServiceKeys(Settings&               settings, const filesystem::path& certificateFile,
                          const filesystem::path& privateKeyFile)
{
    CWebService webService;
    webService.m_certfile = certificateFile.string();
    webService.m_keyfile = privateKeyFile.string();

    // Only the two paths are set, and serviceControl leaves alone what it is not given, so the
    // port and the encryption setting keep their values.
    settings.serviceControl("set", webService);
}
} // namespace

void ControlService::SSLKeysControl(const CSSLKeysControl& input, CSSLKeysControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        auto& settings = *getSettings();

        const auto action = input.m_action.asString();

        if (action == "get")
        {
            output.m_keys = settings.sslKeysControl(input.m_action, input.m_keys);
        }
        else if (action == "reissue")
        {
            reissueWebServiceCertificate(output);
        }
        else if (action == "trust-peer")
        {
            trustPeerCertificate(input.m_peer);
        }
        else if (action == "distrust-peer")
        {
            distrustPeerCertificate(input.m_peer);
        }
        else if (action == "set" && !input.m_keys.isNull())
        {
            const auto keysDirectory = certificatesDirectory();

            // What the interface sends is file content; what the configuration holds, and what
            // loadSslKeys() reads at startup, are paths. So the uploads are written to disk and
            // the resulting paths, not the content, are what gets stored.
            const auto currentKeys = settings.sslKeysControl("get", input.m_keys);
            const auto serverCAFileName = writeCertificate(keysDirectory, "ca.crt",
                                                           input.m_keys.m_cafile.asString(),
                                                           currentKeys.m_cafile.asString());
            const auto serverKeyFileName = writeCertificate(keysDirectory, "server.key",
                                                            input.m_keys.m_keyfile.asString(),
                                                            currentKeys.m_keyfile.asString());
            const auto serverCertFileName = writeCertificate(keysDirectory, "server.crt",
                                                             input.m_keys.m_certfile.asString(),
                                                             currentKeys.m_certfile.asString());

            auto verifyDepth = input.m_keys.m_verify_depth.asInteger();
            auto verifyMode = verifyDepth ? SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE : SSL_VERIFY_NONE;

            CSSLKeys storedKeys(input.m_keys);
            storedKeys.m_cafile = serverCAFileName;
            storedKeys.m_keyfile = serverKeyFileName;
            storedKeys.m_certfile = serverCertFileName;
            settings.sslKeysControl(input.m_action, storedKeys);
            const auto sslKeys = make_shared<SSLKeys>(serverKeyFileName.c_str(),
                                                      serverCertFileName, "",
                                                      serverCAFileName, verifyMode, verifyDepth);
            if (const auto runningServer = server())
            {
                runningServer->setSSLKeys(sslKeys);
            }
        }

        // Separate from the broker's keys, and reached by the same action: the screen holds both,
        // and either can be saved without disturbing the other.
        if (action == "set" && !input.m_web_service_keys.isNull())
        {
            installWebServiceCertificate(input.m_web_service_keys, output);
        }

        // Reported whatever the action was, so that a page which has just installed a certificate
        // shows what is now in place without asking again.
        reportWebServiceKeys(settings, output.m_web_service_keys);
        reportPeerCertificates(output);

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::installWebServiceCertificate(const CWebServiceKeys& keys, CSSLKeysControlResponse& output) const
{
    const auto certificateContent = keys.m_certfile.asString();
    const auto privateKeyContent = keys.m_keyfile.asString();

    // Both together, or neither. A certificate installed over a key that does not match it leaves
    // the interface unable to complete a single handshake, and the page that would fix it is the
    // one that has just stopped answering. Replacing only the certificate at renewal time means
    // sending the key that goes with it, which the administrator has.
    if (certificateContent.empty() || privateKeyContent.empty())
    {
        throw Exception("Installing a certificate for the configuration interface needs both the "
            "certificate and its private key.");
    }

    const auto [certificateFile, privateKeyFile] = webServiceKeyFiles();
    writeCertificate(certificateFile.parent_path(), certificateFile.filename().string(),
                     certificateContent, certificateFile.string());
    writeCertificate(privateKeyFile.parent_path(), privateKeyFile.filename().string(),
                     privateKeyContent, privateKeyFile.string(), true);

    applyWebServiceCertificate(certificateFile, privateKeyFile, output);
}

void ControlService::reissueWebServiceCertificate(CSSLKeysControlResponse& output) const
{
    const auto [certificateFile, privateKeyFile] = webServiceKeyFiles();

    // Reissued rather than created: create() leaves an existing pair alone, which is what makes it
    // safe to call at every start and useless here - this is asked for exactly when the pair that
    // is there is the problem. The previous files are kept beside the new ones.
    String description;
    SelfSignedCertificate::reissue(certificateFile, privateKeyFile,
                                   getSettings()->nodeHostName(), description);

    applyWebServiceCertificate(certificateFile, privateKeyFile, output);
}

void ControlService::trustPeerCertificate(const CPeerCertificate& peer)
{
    const auto name = String(peer.m_name.asString()).trim();
    if (name.empty())
    {
        throw Exception("A trusted node needs a name to be kept under.");
    }

    // The name becomes a file name. The schema's Name type already excludes separators, and this
    // is the second lock on the same door: a name that walked out of the directory would let one
    // API call write a certificate anywhere the service can reach.
    if (name.find_first_of("/\\.") != String::npos)
    {
        throw Exception("A trusted node's name cannot contain dots or path separators.");
    }

    const Buffer certificate(peer.m_certificate.asString());
    if (certificate.empty())
    {
        throw Exception("No certificate was sent for " + name + ".");
    }

    const auto directory = Settings::peerCertificatesDirectory();
    if (!filesystem::exists(directory) && !filesystem::create_directories(directory))
    {
        throw Exception("Unable to create directory " + directory.string());
    }

    const auto certificateFile = directory / (name + ".crt").c_str();
    certificate.saveToFile(certificateFile);

    // Read back before it counts as trusted: a file that OpenSSL cannot parse would be silently
    // skipped when the bundle is loaded, leaving a link that looks verified and is not.
    if (SelfSignedCertificate::describe(certificateFile).empty())
    {
        filesystem::remove(certificateFile);
        throw Exception("What was sent for " + name + " is not a certificate this can read.");
    }

    (void) Settings::buildPeerCertificateBundle();
}

void ControlService::distrustPeerCertificate(const CPeerCertificate& peer)
{
    const auto name = String(peer.m_name.asString()).trim();
    if (name.empty() || name.find_first_of("/\\.") != String::npos)
    {
        throw Exception("That is not a name a trusted node is kept under.");
    }

    const auto certificateFile = Settings::peerCertificatesDirectory() / (name + ".crt").c_str();
    if (!filesystem::exists(certificateFile))
    {
        throw Exception("No trusted node is kept under the name " + name + ".");
    }

    filesystem::remove(certificateFile);
    (void) Settings::buildPeerCertificateBundle();
}

void ControlService::reportPeerCertificates(CSSLKeysControlResponse& output)
{
    output.m_peers.clear();

    error_code errorCode;
    if (const auto directory = Settings::peerCertificatesDirectory();
        filesystem::exists(directory, errorCode))
    {
        for (const auto& entry: filesystem::directory_iterator(directory, errorCode))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".crt")
            {
                continue;
            }

            CPeerCertificate peer;
            peer.m_name = entry.path().stem().string();
            // Described, not sent: a listing of twenty peers does not need twenty certificates in
            // it, and what identifies one at a glance is its fingerprint.
            peer.m_description = SelfSignedCertificate::describe(entry.path());
            output.m_peers.push_back(peer);
        }
    }

    // This node's own certificate, which is what the other nodes have to be given. Reported so
    // that setting up a link does not need a shell on this machine.
    if (const auto [certificateFile, privateKeyFile] = Settings::nodeKeyFiles();
        filesystem::exists(certificateFile, errorCode))
    {
        Buffer certificate;
        certificate.loadFromFile(certificateFile);
        output.m_node_certificate = String(certificate.c_str(), certificate.size());
    }
}

void ControlService::applyWebServiceCertificate(const filesystem::path&  certificateFile,
                                                const filesystem::path&  privateKeyFile,
                                                CSSLKeysControlResponse& output) const
{
    recordWebServiceKeys(*getSettings(), certificateFile, privateKeyFile);

    if (string reason;
        !m_controller->updateControlServiceKeys(certificateFile, privateKeyFile, reason))
    {
        // Installed and recorded either way: what did not happen is handing the keys to the
        // listener. Reported as a note on a successful call rather than as a failure, because
        // calling it a failure would suggest the files are not there.
        output.m_result.m_description = "The certificate is installed. The interface could not "
                                        "take it: " +
                                        reason;
        return;
    }

    // Nothing more to say: SPTK notices that the key files have been replaced and builds a new TLS
    // context for them, so connections made from here on get the certificate above. Connections
    // already open keep the one they were made with, including the one this reply is travelling
    // on - which is what keeps installing a certificate from cutting off the page that did it.
}

void ControlService::LimitsControl(const CLimitsControl& input, CLimitsControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);
        if (input.m_action.asString() == "set")
        {
            CServerLimits serverLimits(input.m_server_limits);
            CQueueLimits  queueLimits(input.m_queue_limits);
            getSettings()->limitsControl(input.m_action, serverLimits, queueLimits);
        }
        else if (input.m_action.asString() == "get")
        {
            getSettings()->limitsControl(input.m_action, output.m_server_limits, output.m_queue_limits);
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

namespace {

/**
 * @brief Pulls a value out of a Redis INFO section.
 *
 * INFO returns one bulk string of "name:value" lines rather than a structured reply, so the
 * field wanted has to be found in it.
 *
 * @param info      Text as INFO returned it.
 * @param fieldName Field to find, without its colon.
 * @return the value, or 0 when the field is absent.
 */
uint64_t redisInfoValue(const string& info, const string& fieldName)
{
    const auto fieldStart = info.find("\n" + fieldName + ":");
    if (fieldStart == string::npos)
    {
        return 0;
    }
    const auto valueStart = fieldStart + fieldName.length() + 2;
    return strtoull(info.c_str() + valueStart, nullptr, 10);
}

/**
 * @brief Whether a Redis host is this machine.
 *
 * Disk figures are only worth showing for a local Redis: for a remote one they would describe
 * this host's filesystem while labelled as the database's, which is worse than showing nothing.
 *
 * Loopback names and addresses are recognised directly; anything else is compared against the
 * addresses this host actually answers on, so a Redis reached by the machine's own hostname or
 * LAN address is still recognised as local.
 */
bool isLocalHost(const string& hostName)
{
    if (hostName.empty() || hostName == "localhost" || hostName == "127.0.0.1" || hostName == "::1")
    {
        return true;
    }

    try
    {
        if (Host(hostName, 0).toString(false) == Host("localhost", 0).toString(false))
        {
            return true;
        }

        constexpr size_t               maxHostNameLength = 256;
        array<char, maxHostNameLength> thisHostName{};
        if (gethostname(thisHostName.data(), static_cast<int>(thisHostName.size() - 1)) == 0 &&
            hostName == string(thisHostName.data()))
        {
            return true;
        }
    }
    catch (const Exception&)
    {
        // An address that cannot be resolved is not one we can call local.
        return false;
    }

    return false;
}

/**
 * @brief The directory Redis keeps its database in, as Redis itself reports it.
 *
 * Asked of the server rather than guessed from the configuration: the two can differ, and only
 * the server knows where it actually writes. CONFIG may be disabled or renamed on a hardened
 * instance, in which case the answer is simply unknown and the disk figures are left out.
 *
 * @return the directory, or empty when it could not be determined.
 */
string redisDataDirectory(const SRedisConnect& connection)
{
    try
    {
        RedisCommand command("CONFIG", "GET");
        command.emplace_back("dir");

        vector<Variant> results;
        connection->executeCommand(command, results);
        // CONFIG GET answers as name/value pairs.
        for (size_t index = 0; index + 1 < results.size(); index += 2)
        {
            if (results[index].asString() == "dir")
            {
                return results[index + 1].asString().c_str();
            }
        }
    }
    catch (const Exception&)
    {
        // Not available: reported as unknown rather than guessed.
        return {};
    }
    return {};
}

} // namespace

void ControlService::GetStatistics(const CGetStatistics&, CGetStatisticsResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto runningServer = server();
        output.m_server_running = runningServer != nullptr;

        // The broker section is left out entirely while the server is stopped. Sending zeroes
        // would describe an idle broker, which is a different thing from no broker at all.
        if (runningServer)
        {
            if (const auto statistics = runningServer->systemStatistics())
            {
                using enum SystemStatistics::SysTopicKind;
                CBrokerStatistics broker;
                broker.m_clients_connected = static_cast<int64_t>(statistics->getValue(BrokerClientsConnected));
                broker.m_clients_disconnected = static_cast<int64_t>(statistics->getValue(BrokerClientsDisconnected));
                broker.m_clients_maximum = static_cast<int64_t>(statistics->getValue(BrokerClientsMaximum));
                broker.m_clients_total = static_cast<int64_t>(statistics->getValue(BrokerClientsTotal));
                broker.m_messages_received = static_cast<int64_t>(statistics->getValue(BrokerMessagesReceived));
                broker.m_messages_sent = static_cast<int64_t>(statistics->getValue(BrokerMessagesSent));
                broker.m_messages_publish_received = static_cast<int64_t>(statistics->getValue(BrokerMessagesPublishReceived));
                broker.m_messages_publish_sent = static_cast<int64_t>(statistics->getValue(BrokerMessagesPublishSent));
                broker.m_messages_publish_dropped = static_cast<int64_t>(statistics->getValue(BrokerMessagesPublishDropped));
                broker.m_messages_retained = static_cast<int64_t>(statistics->getValue(BrokerMessagesRetainedCount));
                broker.m_messages_stored = static_cast<int64_t>(statistics->getValue(BrokerMessagesStored));
                broker.m_queue_receive = static_cast<int64_t>(statistics->getValue(BrokerQueuesReceive));
                broker.m_queue_send = static_cast<int64_t>(statistics->getValue(BrokerQueuesSend));
                broker.m_queue_delivery = static_cast<int64_t>(statistics->getValue(BrokerQueuesDelivery));
                broker.m_bytes_received = static_cast<int64_t>(statistics->getValue(BrokerLoadBytesReceived));
                broker.m_bytes_sent = static_cast<int64_t>(statistics->getValue(BrokerLoadBytesSent));
                broker.m_subscriptions = static_cast<int64_t>(statistics->getValue(BrokerSubscriptionsCount));
                broker.m_uptime_seconds = static_cast<int64_t>(statistics->brokerUptimeSeconds());
                output.m_broker = broker;
            }
        }

        if (const auto snapshot = m_hostMetrics.read();
            snapshot.m_available)
        {
            CHostStatistics host;
            host.m_cpu_percent_total = snapshot.m_cpuPercentTotal;
            host.m_cpu_percent_process = snapshot.m_cpuPercentProcess;
            host.m_cpu_cores = static_cast<int64_t>(snapshot.m_cpuCores);
            host.m_memory_used_by_process = static_cast<int64_t>(snapshot.m_memoryUsedByProcess);
            host.m_memory_used = static_cast<int64_t>(snapshot.m_memoryUsed);
            host.m_memory_available = static_cast<int64_t>(snapshot.m_memoryAvailable);
            host.m_memory_total = static_cast<int64_t>(snapshot.m_memoryTotal);
            host.m_process_uptime_seconds = static_cast<int64_t>(snapshot.m_processUptimeSeconds);
            output.m_host = host;
        }

        // Only when persistence is configured, and only from INFO: it answers for a remote Redis
        // as readily as a local one, and needs no access to its data directory.
        if (getSettings()->m_persistence.m_enabled.asBool())
        {
            CRedisStatistics redis;
            redis.m_connected = false;

            if (const auto redisStorage = runningServer ? runningServer->getRedisStorage() : nullptr)
            {
                redis.m_host = redisStorage->host().toString();

                if (const auto connection = redisStorage->getRedis();
                    connection && connection->isConnected())
                {
                    redis.m_connected = true;

                    // Each figure is attempted on its own. A Redis that refuses one command - or
                    // answers in a way this client cannot read - should cost that one number, not
                    // the whole section.
                    try
                    {
                        vector<Variant> keyCount;
                        connection->executeCommand(RedisCommand("DBSIZE"), keyCount);
                        if (!keyCount.empty())
                        {
                            redis.m_keys = keyCount.front().asInteger();
                        }
                    }
                    catch (const Exception&)
                    {
                        // Left unset, and shown as unknown.
                    }

                    try
                    {
                        // INFO answers with a RESP3 verbatim string, which the Redis client does
                        // not currently parse, so this is expected to fail until it does. Kept
                        // deliberately: the moment the client understands that reply, memory use
                        // appears on the dashboard with no further change here.
                        vector<Variant> results;
                        connection->executeCommand(RedisCommand("INFO", "memory"), results);
                        if (!results.empty())
                        {
                            const auto info = results.front().asString();
                            redis.m_used_memory = static_cast<int64_t>(redisInfoValue(info, "used_memory"));
                            redis.m_used_memory_peak = static_cast<int64_t>(redisInfoValue(info, "used_memory_peak"));
                        }
                    }
                    catch (const Exception&)
                    {
                        // Memory use stays unknown.
                    }

                    // Disk only when both halves are known: that Redis is on this host, and where
                    // it puts its database. Either one missing and the figures would describe the
                    // wrong filesystem.
                    const auto local = isLocalHost(redisStorage->host().hostname());
                    redis.m_is_local = local;
                    if (local)
                    {
                        if (const auto dataDirectory = redisDataDirectory(connection);
                            !dataDirectory.empty())
                        {
                            uint64_t diskAvailable = 0;
                            if (uint64_t diskTotal = 0;
                                HostMetrics::readDiskSpace(dataDirectory, diskAvailable, diskTotal))
                            {
                                redis.m_data_directory = dataDirectory;
                                redis.m_disk_available = static_cast<int64_t>(diskAvailable);
                                redis.m_disk_total = static_cast<int64_t>(diskTotal);
                            }
                        }
                    }
                }
            }

            output.m_redis = redis;
        }

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::ServerControl(const CServerControl& input, CServerControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        if (const auto action = input.m_action.asString();
            action == "start")
        {
            // A server that will not start is reported, not thrown: the caller is the interface,
            // and the whole point of hosting it outside the server is that it survives to show
            // the reason and let the configuration be corrected.
            if (string reason;
                !m_controller->startServing(reason))
            {
                output.m_message = reason;
            }
        }
        else if (action == "stop")
        {
            m_controller->stopServing();
        }
        else if (action == "reload-extensions")
        {
            const auto report = m_controller->reloadExtensions();
            if (report.failed())
            {
                output.m_result.m_success = false;
                output.m_result.m_description = report.m_problems.join("\n");
                return;
            }

            // Said back even when it is empty, because "nothing had changed" is the answer an
            // operator who edited the wrong file needs, and silence reads as success.
            output.m_message = report.m_notes.empty() ? String("No extension settings had changed.")
                                                      : report.m_notes.join("\n");
        }
        else if (action != "status")
        {
            throw Exception("Unknown action: " + action);
        }

        output.m_running = m_controller->isServing();
        output.m_version = Server::getVersion();
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_running = m_controller->isServing();
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        output.m_running = m_controller->isServing();
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::ServiceControl(const CServiceControl& input, CServiceControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto previousServicePort = getSettings()->m_web_service.m_listener_port.asInteger();
        const auto wasEncrypted = getSettings()->m_web_service.m_encrypted.asBool();
        output.m_web_service = getSettings()->serviceControl(input.m_action, input.m_web_service);

        // Moved rather than left for a restart to pick up, exactly as the initial setup does it:
        // the same setting behaving differently depending on which page changed it is a trap.
        // Only the accepting socket changes, so this request is still answered on the connection
        // it arrived on, and a port that cannot be taken leaves the interface where it was.
        output.m_service_restart_required = false;
        if (const auto servicePort = output.m_web_service.m_listener_port.asInteger();
            servicePort != previousServicePort)
        {
            if (string reason;
                !m_controller->moveControlService(static_cast<uint16_t>(servicePort), reason))
            {
                output.m_service_restart_required = true;
                output.m_message = "The configuration interface could not move to port " +
                                   to_string(servicePort) + ": " + reason;
            }
        }

        // The scheme, for the same reason and in the same way. Whether a connection is encrypted
        // is decided by the socket it arrived on, so this replaces that socket; the connection
        // carrying this reply is not one of them and is served to the end.
        if (const auto encrypted = output.m_web_service.m_encrypted.asBool();
            encrypted != wasEncrypted)
        {
            if (string reason;
                m_controller->setControlServiceEncrypted(encrypted, reason))
            {
                // Named rather than implied: every later request has to go to the other scheme,
                // and a page that does not know that is a page that has stopped working.
                output.m_message = "The configuration interface is now served over " +
                                   String(encrypted ? "https" : "http") + ".";
            }
            else
            {
                output.m_service_restart_required = true;
                output.m_message = "The configuration interface could not change scheme: " + reason;
            }
        }

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

void ControlService::InitialSetupControl(const CInitialSetupControl& input, CInitialSetupControlResponse& output,
                                         HttpAuthentication*         authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto settings = getSettings();

        if (const auto action = input.m_action.asString();
            action == "get")
        {
            output.m_setup = settings->initialSetup();
            output.m_running = m_controller->isServing();
        }
        else if (action == "set")
        {
            const auto previousServicePort = settings->m_web_service.m_listener_port.asInteger();

            requireReachableRedis(input.m_setup);
            settings->applyInitialSetup(input.m_setup);

            // Anything that went wrong after the configuration was written. Collected rather than
            // thrown, and collected rather than assigned: the interface and the server can each
            // fail on their own, and being told about only one of them is how the other gets
            // missed.
            Strings problems;

            // The interface is moved rather than left for a restart to pick up, so that the page
            // which just changed the port can send the browser straight to it. Only the accepting
            // socket changes, so this very request is still answered on the connection it arrived
            // on. A port that cannot be taken leaves the interface where it was, and says so.
            //
            // Asked for unconditionally rather than only when the port changed, because the
            // address can change without it: a first setup has just given the administrator a
            // password, and the interface stops answering on the loopback address alone. When
            // neither has changed this costs nothing - the listener compares both and returns.
            output.m_service_restart_required = false;
            const auto servicePort = settings->m_web_service.m_listener_port.asInteger();
            if (string reason;
                !m_controller->moveControlService(static_cast<uint16_t>(servicePort), reason))
            {
                const auto portChanged = servicePort != previousServicePort;
                output.m_service_restart_required = portChanged;
                problems.push_back(portChanged
                                       ? "The configuration interface could not move to port " +
                                             to_string(servicePort) + ": " + reason
                                       : "The configuration interface could not start accepting "
                                         "connections from the network: " + reason);
            }

            // Restarted rather than reconfigured in place: every listener, bridge, and storage
            // connection the old configuration built has to go, and building them again from
            // the new one is what the server does at startup anyway.
            m_controller->stopServing();
            if (string reason;
                !m_controller->startServing(reason))
            {
                problems.push_back(reason);
            }

            if (!problems.empty())
            {
                output.m_message = problems.join("\n");
            }

            // What is now configured, which is what was asked for: applyInitialSetup() either
            // applied all of it or threw and applied none of it. The password is not echoed -
            // the caller is the only party that knows it, and it has no business in a reply that
            // may be logged along the way.
            output.m_setup = input.m_setup;
            output.m_setup.m_admin_password.clear();
            output.m_running = m_controller->isServing();
        }
        else
        {
            throw Exception("Unknown action: " + action);
        }

        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_running = m_controller->isServing();
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: anything thrown here that is not an sptk::Exception would otherwise escape
        // the handler and leave the caller waiting for a response that never comes, rather than
        // showing the reason.
        output.m_running = m_controller->isServing();
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}

String ControlService::findBridgePassword(const int bridgeId) const
{
    String existingPassword;
    for (const auto& bridge: getSettings()->m_bridges)
    {
        if (bridge.m_id.asInteger() == bridgeId)
        {
            return bridge.m_password.asString();
        }
    }
    throw Exception("Can't find the bridge");
}

void ControlService::BridgeControl(const CBridgeControl& input, CBridgeControlResponse& output, HttpAuthentication* authentication)
{
    try
    {
        CUser user;
        authenticate(authentication, user);

        const auto action = input.m_action.asString();
        const auto settings = getSettings();

        // Bridge definitions are saved whether or not MQTT is running; connecting to the peer is
        // what needs a server, and a stopped one connects what it finds configured when started.
        const auto runningServer = server();

        CBridge bridgeSettings(input.m_bridge);
        auto    bridgeId = bridgeSettings.m_id.asInteger();

        if (action == "add" || action == "modify")
        {
            if (const auto newPassword = bridgeSettings.m_password.asString();
                newPassword == "*****")
            {
                // The password is passed as all-stars, finding the existing password:
                bridgeSettings.m_password = findBridgePassword(bridgeId);
            }

            bridgeId = static_cast<int>(settings->bridgeControl(action, bridgeSettings));
            bridgeSettings.m_id = bridgeId;
            if (runningServer)
            {
                runningServer->cluster()->disconnectClusterNode(bridgeSettings.m_node_name.asString());
                runningServer->cluster()->connectClusterNode(bridgeSettings.m_node_name.asString());
            }
        }
        else if (action == "remove")
        {
            settings->bridgeControl(action, bridgeSettings);
            if (runningServer)
            {
                runningServer->cluster()->disconnectClusterNode(bridgeSettings.m_node_name.asString());
            }
        }
        else if (action == "list")
        {
            for (const auto& bridge: getSettings()->m_bridges)
            {
                CBridge bridgeInfo(bridge);
                bridgeInfo.m_password = "*****";
                output.m_list.push_back(bridgeInfo);
            }
        }
        else if (action == "apply")
        {
            // Adding or changing a bridge only writes the configuration; the connections are
            // built when the bridges are started. This rebuilds them from the configuration as
            // it now stands, so a change takes effect without restarting the server.
            if (const auto aServer = server())
            {
                aServer->restartBridges();
            }
        }
        output.m_result.m_success = true;
    }
    catch (const Exception& e)
    {
        output.m_result.m_success = false;
        output.m_result.m_description = e.message();
    }
    catch (const std::exception& e)
    {
        // Backstop: BridgeManager reports rejections by throwing, and anything that is not an
        // sptk::Exception would otherwise escape this handler and leave the caller waiting for
        // a response that never arrives, instead of being shown the reason.
        output.m_result.m_success = false;
        output.m_result.m_description = e.what();
    }
}
