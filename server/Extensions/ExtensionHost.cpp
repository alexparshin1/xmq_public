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

#include "ExtensionHost.h"

#include <sptk5/xdoc/Document.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <ranges>
#include <utility>

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// Counts a call while it is inside an extension, so that switching that extension off waits for it
/// rather than pulling stop() out from under it.
class InFlight
{
public:
    explicit InFlight(std::atomic<int>& counter)
        : m_counter(counter)
    {
        m_counter.fetch_add(1, std::memory_order_acquire);
    }

    ~InFlight()
    {
        m_counter.fetch_sub(1, std::memory_order_release);
    }

    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;

private:
    std::atomic<int>& m_counter;
};

} // namespace

namespace {

#ifndef _WIN32
void* openLibrary(const filesystem::path& path)
{
    // Local, so an extension's symbols cannot satisfy anyone else's undefined ones, and NOW, so a
    // missing symbol is a refusal to load rather than a crash on the first call to it.
    return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* findSymbol(void* handle, const char* name)
{
    return dlsym(handle, name);
}
void closeLibrary(void* handle)
{
    dlclose(handle);
}
string libraryError()
{
    const char* e = dlerror();
    return e == nullptr ? "unknown error" : e;
}
#else
void* openLibrary(const filesystem::path& path)
{
    // LOAD_WITH_ALTERED_SEARCH_PATH so that an extension's own dependencies are looked for beside
    // it rather than beside xmq_server.exe. An authentication extension ships with its directory
    // client's DLLs, and without this the extension loads only if those are dropped into the
    // broker's own directory - which is how a working extension appears not to exist.
    // The path must be absolute for the flag to mean anything, and the configuration gives one.
    return reinterpret_cast<void*>(
        LoadLibraryExW(absolute(path).wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

void* findSymbol(void* handle, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}

void closeLibrary(void* handle)
{
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
}

string libraryError()
{
    const auto code = GetLastError();
    LPSTR      text = nullptr;
    const auto length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    if (length == 0 || text == nullptr)
    {
        return "error " + to_string(code);
    }
    // Windows ends these with a newline, which reads badly in the middle of a log line.
    string message(text, length);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
    {
        message.pop_back();
    }
    return message + " (" + to_string(code) + ")";
}
#endif

/// The authenticator an extension declared, or nullptr.
const xmq_authenticator* authenticatorOfTable(const xmq_extension& table)
{
    if ((table.capabilities & XMQ_CAP_AUTHENTICATOR) == 0)
    {
        return nullptr;
    }
    return table.authenticator != nullptr && table.authenticator->authenticate != nullptr
               ? table.authenticator
               : nullptr;
}

/// The authorizer an extension declared, or nullptr.
const xmq_authorizer* authorizerOfTable(const xmq_extension& table)
{
    if ((table.capabilities & XMQ_CAP_AUTHORIZER) == 0)
    {
        return nullptr;
    }
    return table.authorizer != nullptr && table.authorizer->authorize != nullptr ? table.authorizer : nullptr;
}

uint64_t nowMicroseconds()
{
    return static_cast<uint64_t>(
        chrono::duration_cast<chrono::microseconds>(chrono::system_clock::now().time_since_epoch()).count());
}

LogPriority toLogPriority(const xmq_log_priority priority)
{
    using enum LogPriority;
    switch (priority)
    {
        case XMQ_LOG_DEBUG:
            return Debug;
        case XMQ_LOG_INFO:
            return Info;
        case XMQ_LOG_WARNING:
            return Warning;
        case XMQ_LOG_ERROR:
            return Error;
    }
    return Info;
}

} // namespace

ExtensionHost::ExtensionHost(LogEngine& logEngine, string brokerVersion)
    : m_logger(logEngine, "[extensions] ")
      , m_brokerVersion(std::move(brokerVersion))
{
}

ExtensionHost::~ExtensionHost()
{
    stop();
}

void ExtensionHost::hostLog(void* context, const xmq_log_priority priority, const char* message)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || loaded->m_host == nullptr || message == nullptr)
    {
        return;
    }
    loaded->m_host->m_logger.log(toLogPriority(priority), loaded->m_name + ": " + message);

    // Kept so that a call answering only yes or no can still be asked why. Without the extension's
    // own name in front: the caller puts that back when it composes the line.
    if (priority == XMQ_LOG_ERROR)
    {
        const std::scoped_lock lock(loaded->m_lastErrorLock);
        loaded->m_lastError = message;
    }
}

/// The last error this extension logged, and forgets it. Called around one call, so what comes back
/// is what that call said.
std::string ExtensionHost::takeLastError(Loaded& loaded)
{
    const std::scoped_lock lock(loaded.m_lastErrorLock);
    return std::exchange(loaded.m_lastError, std::string{});
}

namespace {

/// Hands a string to an extension the way the ABI says: copied into its buffer, NUL-terminated and
/// cut to fit, with the whole length returned so a caller whose buffer was short can ask again.
int64_t copyOut(const std::string& value, char* buffer, const size_t bufferSize)
{
    if (buffer != nullptr && bufferSize > 0)
    {
        const auto copied = std::min(value.size(), bufferSize - 1);
        std::memcpy(buffer, value.data(), copied);
        buffer[copied] = '\0';
    }
    return static_cast<int64_t>(value.size());
}

} // namespace

int64_t ExtensionHost::hostSetting(void* context, const char* key, char* buffer, const size_t bufferSize)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || key == nullptr)
    {
        return -1;
    }
    std::string value;
    {
        const std::scoped_lock lock(loaded->m_settingsLock);
        const auto             setting = loaded->m_settings.find(key);
        if (setting == loaded->m_settings.end())
        {
            return -1;
        }
        value = setting->second;
    }
    return copyOut(value, buffer, bufferSize);
}

int64_t ExtensionHost::hostUserDatabaseUri(void* context, char* buffer, const size_t bufferSize)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || loaded->m_host == nullptr)
    {
        return -1;
    }
    std::string uri;
    {
        const std::scoped_lock lock(loaded->m_host->m_userDatabaseUriLock);
        uri = loaded->m_host->m_userDatabaseUri;
    }
    // A broker whose accounts are not in a database at all: -1 rather than an empty string, so an
    // extension can tell "nowhere" from "somewhere I could not read".
    return uri.empty() ? -1 : copyOut(uri, buffer, bufferSize);
}

void ExtensionHost::userDatabaseUri(std::string uri)
{
    {
        const std::scoped_lock lock(m_userDatabaseUriLock);
        if (m_userDatabaseUri == uri)
        {
            return;
        }
        m_userDatabaseUri = std::move(uri);
    }

    // The accounts have moved, and an authenticator that asked for the address at start() is still
    // holding a connection to where they used to be. Nothing else would tell it: reloadSettings()
    // only disturbs an extension whose own settings changed, and this address is deliberately not
    // one of those. reload() is the right call here and not accounts_changed() - the store itself
    // is different, so the connection has to be opened again, not a cache dropped.
    for (const auto& loaded: m_loaded)
    {
        if (!loaded->m_started || loaded->m_table == nullptr || loaded->m_table->reload == nullptr)
        {
            continue;
        }
        takeLastError(*loaded);
        if (loaded->m_table->reload(loaded->m_instance) != XMQ_OK)
        {
            const auto reason = takeLastError(*loaded);
            m_logger.error(format("{}: the account database moved and it could not follow{}",
                                  loaded->m_name, reason.empty() ? "" : ": " + reason));
        }
    }
}

void ExtensionHost::accountsChanged()
{
    for (const auto& loaded: m_loaded)
    {
        if (!loaded->m_started || loaded->m_table == nullptr || loaded->m_table->accounts_changed == nullptr)
        {
            // Caches nothing. Not worth a note anywhere: an extension that does not cache has
            // nothing to forget, and one that authenticates against something the broker does not
            // edit was never going to be told.
            continue;
        }
        loaded->m_table->accounts_changed(loaded->m_instance);
    }
}

xmq_str ExtensionHost::hostBrokerVersion(void* context)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || loaded->m_host == nullptr)
    {
        return {.data = nullptr, .length = 0};
    }
    const auto& version = loaded->m_host->m_brokerVersion;
    return {.data = version.c_str(), .length = version.size()};
}

uint32_t ExtensionHost::attributeBit(const string_view name)
{
    // One bit per name this broker publishes. A name that is not here is one an extension asked for
    // and this broker does not have, which is not an error - it is exactly the case the mechanism
    // exists to make survivable.
    if (name == remoteAddressAttribute)
    {
        return 1U << 0U;
    }
    return 0;
}

void ExtensionHost::hostWantEventAttributes(void* context, const char* names)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || loaded->m_host == nullptr || names == nullptr)
    {
        return;
    }

    uint32_t wanted = 0;
    Strings  unknown;
    for (const auto& name: Strings(String(names), ","))
    {
        const auto trimmed = name.trim();
        if (trimmed.empty())
        {
            continue;
        }
        if (const auto bit = attributeBit(string_view(trimmed.c_str(), trimmed.size()));
            bit != 0)
        {
            wanted |= bit;
        }
        else
        {
            unknown.push_back(trimmed);
        }
    }

    if (!unknown.empty())
    {
        // Said rather than ignored: an extension asking for a name it spelled wrong and one asking
        // for a fact this broker does not publish look identical from inside the extension, and
        // only the log can tell the author which of the two happened.
        loaded->m_host->m_logger.warning(loaded->m_name + ": this broker publishes no event attribute named " +
                                         unknown.join(", "));
    }

    // Or-ed, never assigned: two extensions may want different things, and the second must not
    // take away what the first asked for.
    loaded->m_host->m_wantedAttributes.fetch_or(wanted, std::memory_order_relaxed);
}

xmq_str ExtensionHost::hostEventAttribute(void* context, const xmq_event* event, const char* name)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded == nullptr || loaded->m_host == nullptr || event == nullptr || name == nullptr)
    {
        return {.data = nullptr, .length = 0};
    }

    // Only for the event being delivered. An extension that kept the pointer and asked afterwards
    // gets nothing, which is the truth: the strings behind it are gone.
    const auto* host = loaded->m_host;
    if (event != host->m_deliveringEvent || host->m_deliveringAttributes == nullptr)
    {
        return {.data = nullptr, .length = 0};
    }

    for (const auto& [attributeName, value]: *host->m_deliveringAttributes)
    {
        if (attributeName == name)
        {
            return {.data = value.c_str(), .length = value.size()};
        }
    }
    return {.data = nullptr, .length = 0};
}

void ExtensionHost::hostInvalidateAcl(void* context)
{
    auto* loaded = static_cast<Loaded*>(context);
    if (loaded != nullptr && loaded->m_host != nullptr)
    {
        loaded->m_host->invalidateAcl();
    }
}

unique_ptr<ExtensionHost::Loaded> ExtensionHost::load(const Configured& configured)
{
    auto loaded = make_unique<Loaded>();
    loaded->m_name = configured.m_name;
    loaded->m_library = configured.m_library;
    loaded->m_settings = configured.m_settings;
    loaded->m_required = configured.m_required;
    loaded->m_source = configured.m_source;
    loaded->m_host = this;

    loaded->m_handle = openLibrary(configured.m_library);
    if (loaded->m_handle == nullptr)
    {
        m_logger.error(format("{}: cannot load {}: {}", configured.m_name,
                              configured.m_library.string(), libraryError()));
        return nullptr;
    }

    auto* describe = reinterpret_cast<xmq_extension_describe_fn>(
        findSymbol(loaded->m_handle, XMQ_EXTENSION_DESCRIBE_SYMBOL));
    if (describe == nullptr)
    {
        m_logger.error(format("{}: {} exports no {}, so it is not an XMQ extension",
                              configured.m_name, configured.m_library.string(),
                              XMQ_EXTENSION_DESCRIBE_SYMBOL));
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    loaded->m_table = describe(XMQ_EXTENSION_ABI_MAJOR, XMQ_EXTENSION_ABI_MINOR);
    if (loaded->m_table == nullptr)
    {
        m_logger.error(format("{}: declined ABI {}.{}, so it was built for another broker",
                              configured.m_name, XMQ_EXTENSION_ABI_MAJOR, XMQ_EXTENSION_ABI_MINOR));
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    // An extension compiled against a larger table would have the broker read past what it wrote,
    // and one compiled against a smaller table was built for an ABI before 1.0, whose layout this
    // broker does not read. ABI 1.0 is the first; later minors will append, and relax the second.
    if (loaded->m_table->struct_size != sizeof(xmq_extension))
    {
        m_logger.error(format("{}: describes itself with {} bytes where this broker knows {} - rebuild "
                              "it against this broker's xmq_extension.h",
                              configured.m_name, loaded->m_table->struct_size, sizeof(xmq_extension)));
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    if (loaded->m_table->create == nullptr || loaded->m_table->destroy == nullptr)
    {
        m_logger.error(configured.m_name + ": has no create/destroy pair");
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    const string declared = loaded->m_table->name == nullptr ? string{} : loaded->m_table->name;
    if (declared != configured.m_name)
    {
        // Loading it anyway would mean the configuration names one extension and the broker runs
        // another, which is exactly the confusion a name is for. Both names and the file, because
        // the entry is usually right and the spelling wrong - and say it was not loaded, or an
        // author reads one error line and still waits for the extension to answer.
        m_logger.error(format("{}: not loaded - the library in {} calls itself '{}'. Make the two "
                              "names match, in {} or in the extension.",
                              configured.m_name, configured.m_library.string(), declared,
                              configured.m_source.string()));
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    loaded->m_hostTable = {.struct_size = sizeof(xmq_host),
                           .context = loaded.get(),
                           .log = &ExtensionHost::hostLog,
                           .setting = &ExtensionHost::hostSetting,
                           .broker_version = &ExtensionHost::hostBrokerVersion,
                           .user_database_uri = &ExtensionHost::hostUserDatabaseUri,
                           .invalidate_acl = &ExtensionHost::hostInvalidateAcl,
                           .want_event_attributes = &ExtensionHost::hostWantEventAttributes,
                           .event_attribute = &ExtensionHost::hostEventAttribute};

    loaded->m_instance = loaded->m_table->create(&loaded->m_hostTable);
    if (loaded->m_instance == nullptr)
    {
        m_logger.error(configured.m_name + ": failed to create an instance");
        closeLibrary(loaded->m_handle);
        return nullptr;
    }

    return loaded;
}

filesystem::path ExtensionHost::configurationPathFor(const filesystem::path& configurationPath)
{
    if (configurationPath.empty())
    {
        return {};
    }
    return filesystem::path(configurationPath).replace_filename("xmq_extensions.conf");
}

filesystem::path ExtensionHost::configurationDirectoryFor(const filesystem::path& configurationPath)
{
    if (configurationPath.empty())
    {
        return {};
    }
    return filesystem::path(configurationPath).replace_filename("xmq_extensions.d");
}

void ExtensionHost::createConfigurationFragments(const filesystem::path& configurationPath,
                                                 LogEngine&              logEngine)
{
    const auto directory = configurationDirectoryFor(configurationPath);
    error_code errorCode;
    if (directory.empty() || !filesystem::is_directory(directory, errorCode))
    {
        return;
    }

    const Logger logger(logEngine, "[extensions] ");
    for (const auto& entry: filesystem::directory_iterator(directory, errorCode))
    {
        if (!entry.is_regular_file(errorCode) || entry.path().extension() != ".template")
        {
            continue;
        }

        auto fragment = entry.path();
        fragment.replace_extension(); // <name>.conf.template -> <name>.conf
        if (fragment.extension() != ".conf" || filesystem::exists(fragment, errorCode))
        {
            continue;
        }

        filesystem::copy_file(entry.path(), fragment, errorCode);
        if (errorCode)
        {
            // Reported and stepped over, never fatal: the extension it would have configured is
            // simply not configured, and a broker that refuses to start over a file it could not
            // write is worse than a broker running with one extension fewer.
            logger.error(format("{}: {}, so the extension it configures is not loaded",
                                fragment.string(), errorCode.message()));
            continue;
        }

        // The same permissions the installed templates have: a fragment can hold a password.
        filesystem::permissions(fragment,
                                filesystem::perms::owner_read | filesystem::perms::owner_write |
                                filesystem::perms::group_read,
                                errorCode);
        logger.info(format("{} was not found, so a starting one was created (copied from {})",
                           fragment.string(), entry.path().string()));
    }
}

void ExtensionHost::readConfigurationFile(const filesystem::path&        path, const Logger& logger,
                                          vector<Configured>&            extensions,
                                          map<string, filesystem::path>& seenIn)
{
    try
    {
        Buffer content;
        content.loadFromFile(path);

        xdoc::Document document;
        document.load(content);

        for (const auto& node: document.root()->nodes("extensions"))
        {
            Configured configured;
            configured.m_name = node->getString("name").trim().c_str();
            configured.m_library = node->getString("library").trim().c_str();

            // Recorded rather than skipped. It used to be dropped here, which made a switched-off
            // extension invisible to the broker - and so absent from a screen that promises to list
            // every one of them, switched off included.
            if (const auto enabled = node->findFirst("enabled", xdoc::SearchMode::Recursive))
            {
                configured.m_enabled = enabled->getBoolean();
            }
            if (configured.m_name.empty() || configured.m_library.empty())
            {
                logger.error(path.string() + ": an entry has no name or no library, skipping it");
                continue;
            }

            // Read after 'enabled', so switching an extension off is enough on its own: an entry
            // that is both required and disabled is a contradiction the operator has already
            // resolved by disabling it.
            if (const auto required = node->findFirst("required", xdoc::SearchMode::Recursive))
            {
                configured.m_required = required->getBoolean();
            }

            // "_comment" keys are how this file is documented to whoever opens it, and they are
            // not settings. Dropped rather than handed to the extension, which would otherwise see
            // a setting it never defined.
            configured.m_settings.erase("_comment");

            if (const auto settings = node->findFirst("settings", xdoc::SearchMode::Recursive))
            {
                for (const auto& setting: settings->nodes())
                {
                    configured.m_settings[std::string(setting->getName())] = setting->getString().c_str();
                }
            }
            // The first entry with a name wins, and the second is refused rather than loaded beside
            // it. Two entries of one name are two instances of the library - two start()s, two of
            // whatever it holds open, and everything it writes twice - which reads as the broker
            // having gone wrong rather than as a configuration that says one thing twice.
            //
            // Easy to arrive at now that there are two places to configure from: an entry left in
            // xmq_extensions.conf and the fragment an install put beside it say the same thing.
            if (const auto seen = seenIn.find(configured.m_name);
                seen != seenIn.end())
            {
                logger.error(format("{}: '{}' is already configured in {}, ignoring this one",
                                    path.string(), configured.m_name, seen->second.string()));
                continue;
            }
            seenIn.emplace(configured.m_name, path);

            configured.m_source = path;
            extensions.push_back(std::move(configured));
        }
    }
    catch (const Exception& e)
    {
        // Reported and ignored: a broker that will not start because an extension list is malformed
        // is a broker held hostage by an optional feature. One bad file also costs only itself -
        // the others in the directory are still read.
        logger.error(format("{}: {}, ignored", path.string(), e.what()));
    }
}

vector<ExtensionHost::Configured> ExtensionHost::readConfiguration(const filesystem::path& configurationPath,
                                                                   LogEngine&              logEngine)
{
    vector<Configured> extensions;
    const Logger       logger(logEngine, "[extensions] ");
    error_code         errorCode;

    /// Which file each name came from, so a repeat can say what it collided with.
    map<string, filesystem::path> seenIn;

    // The single file first, then the directory. Both, because an installation with one extension
    // is easier to read as one file, and an installation whose extensions each install their own
    // fragment cannot be made to share one - two packages editing one JSON file is exactly the
    // merge nobody wants to do by hand.
    if (const auto path = configurationPathFor(configurationPath);
        !path.empty() && filesystem::exists(path, errorCode))
    {
        readConfigurationFile(path, logger, extensions, seenIn);
    }

    // Before the directory is read, not after: a fragment created here is one of the fragments
    // this call is meant to find.
    createConfigurationFragments(configurationPath, logEngine);

    const auto directory = configurationDirectoryFor(configurationPath);
    if (directory.empty() || !filesystem::is_directory(directory, errorCode))
    {
        return extensions;
    }

    // Sorted by name, because the order extensions are asked in is part of what they mean: the
    // broker stops at the first authenticator that answers. Directory iteration order is whatever
    // the filesystem feels like, which would make that order differ between two machines holding
    // the same files. Hence the convention of a numeric prefix - 10-, 20- - in the name.
    vector<filesystem::path> fragments;
    for (const auto& entry: filesystem::directory_iterator(directory, errorCode))
    {
        if (entry.is_regular_file(errorCode) && entry.path().extension() == ".conf")
        {
            fragments.push_back(entry.path());
        }
    }
    ranges::sort(fragments);

    for (const auto& fragment: fragments)
    {
        readConfigurationFile(fragment, logger, extensions, seenIn);
    }

    return extensions;
}

ExtensionHost::Report ExtensionHost::reloadSettings(const vector<Configured>& extensions)
{
    Report report;

    for (const auto& configured: extensions)
    {
        const auto loaded = ranges::find_if(m_loaded, [&configured](const auto& candidate)
        {
            return candidate->m_name == configured.m_name;
        });
        if (loaded == m_loaded.end() || !(*loaded)->m_started)
        {
            // Not loaded, so there is nothing here to reconfigure. Loading it is the other half of
            // this and is not done here - see the note on the declaration.
            continue;
        }

        // Kept, because the new ones are put in place before the extension is asked and have to
        // come back out if it says no. An extension that refused a change and is then described
        // with the refused values tells the operator it accepted them - which is what happened
        // here until it was found from the screen: navigate away, come back, and a rejected
        // database URI was still sitting in the field as though it had taken.
        std::map<std::string, std::string> previous;
        {
            const std::scoped_lock lock((*loaded)->m_settingsLock);
            if ((*loaded)->m_settings == configured.m_settings)
            {
                continue; // unchanged: an extension that has not changed is not disturbed at all
            }
            previous = (*loaded)->m_settings;
            (*loaded)->m_settings = configured.m_settings;
        }

        // Told after the settings are in place, never before: the callback's whole job is to read
        // them, and an extension that read the old ones would apply the change it was told about
        // by not applying it.
        if ((*loaded)->m_table->reload == nullptr)
        {
            report.m_notes.push_back(configured.m_name +
                                     ": settings changed, but it does not take them while running - "
                                     "restart the broker to apply them");
            continue;
        }

        takeLastError(**loaded); // anything older than this call must not be read as its reason
        if ((*loaded)->m_table->reload((*loaded)->m_instance) != XMQ_OK)
        {
            const auto reason = takeLastError(**loaded);
            {
                const std::scoped_lock lock((*loaded)->m_settingsLock);
                (*loaded)->m_settings = previous;
            }
            // The file too, or the refused values come back at the next start - and the entry a
            // restart would read is not what the running broker is using. The values put back are
            // the ones held in memory, so a secret goes back whole rather than as its mask.
            // The verdict on its own line, the extension's own account of it below: the two
            // answer different questions, and one paragraph makes the reader find the seam.
            report.m_problems.push_back(
                configured.m_name + ": refused the new settings and keeps what it had." +
                (reason.empty() ? string{} : "\n\n" + reason));
            report += writeSettings(configured.m_name, previous);
            continue;
        }
        report.m_notes.push_back(configured.m_name + ": settings applied");
    }

    // Switched on: an entry that names an extension nothing has loaded, or one that was switched
    // off earlier. readConfiguration() drops disabled entries, so everything here is meant to run.
    for (const auto& configured: extensions)
    {
        const auto known = ranges::find_if(m_loaded, [&configured](const auto& candidate)
        {
            return candidate->m_name == configured.m_name;
        });
        if (known == m_loaded.end() || !(*known)->m_started)
        {
            report += enable(configured);
        }
    }

    // Switched off: loaded and running, but the configuration no longer asks for it - either the
    // entry is gone or it now says enabled: false, and both mean the same thing here.
    vector<string> departed;
    for (const auto& loaded: m_loaded)
    {
        if (!loaded->m_started)
        {
            continue;
        }
        if (ranges::none_of(extensions, [&loaded](const auto& configured)
        {
            return configured.m_name == loaded->m_name;
        }))
        {
            departed.push_back(loaded->m_name);
        }
    }
    for (const auto& name: departed)
    {
        report += disable(name);
    }

    for (const auto& line: report.m_notes)
    {
        m_logger.info(line);
    }
    for (const auto& line: report.m_problems)
    {
        m_logger.error(line);
    }
    return report;
}

void ExtensionHost::start(const vector<Configured>& extensions)
{
    if (extensions.empty())
    {
        return;
    }

    // Named rather than counted, because the operator has to know which one to repair, and a
    // broker that says "an extension is missing" has told them nothing they can act on.
    Strings missingRequired;

    for (const auto& configured: extensions)
    {
        if (!configured.m_enabled)
        {
            // Its library is never opened. Remembered, so the screen can show it and switch it on.
            m_configuredOnly.push_back(configured);
            continue;
        }

        auto loaded = load(configured);
        if (!loaded)
        {
            // load() has said why. A broken optional extension does not stop the broker; a broken
            // required one does, and the two are told apart here rather than in load().
            if (configured.m_required)
            {
                missingRequired.push_back(configured.m_name);
            }
            continue;
        }

        if (loaded->m_table->start != nullptr && loaded->m_table->start(loaded->m_instance) != XMQ_OK)
        {
            m_logger.error(loaded->m_name + ": failed to start, unloading it");
            unload(*loaded);
            if (configured.m_required)
            {
                missingRequired.push_back(configured.m_name);
            }
            continue;
        }
        loaded->m_started = true;

        m_logger.info(format("{} {} loaded from {}", loaded->m_name,
                             loaded->m_table->version == nullptr ? "" : loaded->m_table->version,
                             loaded->m_library.string()));

        m_loaded.push_back(std::move(loaded));
    }

    // One place decides what is on and which capabilities are live, so that starting up and
    // switching one off later cannot end up disagreeing about either.
    {
        const std::scoped_lock lock(m_loadedLock);
        publishActiveUnlocked();
    }

    // Before any thread is started: a broker that will not serve has no use for an event thread or
    // an authentication pool, and the destructor has less to unwind. Everything loaded so far is
    // stopped and unloaded by ~ExtensionHost when this leaves the Server constructor.
    if (!missingRequired.empty())
    {
        throw Exception("required extension" + String(missingRequired.size() > 1 ? "s " : " ") +
                        missingRequired.join(", ") +
                        " did not start; the broker will not serve MQTT until it does");
    }

    startWorkerThreads();
}

void ExtensionHost::startWorkerThreads()
{
    // Guarded by what is already running rather than by a flag: switching a second observer on must
    // not start a second delivery thread, and an extension switched on long after start-up must get
    // one if start-up had no reason to.
    if (m_watching.load(std::memory_order_relaxed) && !m_deliveryThread.joinable())
    {
        m_deliveryThread = thread(&ExtensionHost::deliveryThread, this);
    }

    // The same pool serves resolveGroup(), which is also allowed to block and also happens once
    // per connection - so an extension that only authorizes still needs it.
    if ((m_authenticating.load(std::memory_order_relaxed) ||
         m_authorizing.load(std::memory_order_relaxed)) &&
        m_authenticationThreads.empty())
    {
        // Sized to the machine, because the work is now on every connection: a database round trip
        // and a costed KDF. Four was the figure from when an authenticator was an unusual thing to
        // have, and it put a ceiling of some thousands of connections a second on the whole broker.
        const auto cores = std::thread::hardware_concurrency();
        const auto threads = std::clamp<size_t>(cores == 0 ? MinAuthenticationThreads : cores,
                                                MinAuthenticationThreads, MaxAuthenticationThreads);
        for (size_t i = 0; i < threads; ++i)
        {
            m_authenticationThreads.emplace_back(&ExtensionHost::authenticationThread, this);
        }
        m_maxQueuedAuthentications.store(std::min(threads * QueuedAuthenticationsPerThread,
                                                  MaxQueuedAuthentications),
                                         std::memory_order_relaxed);
    }
}

void ExtensionHost::stop()
{
    if (m_terminated.exchange(true))
    {
        return;
    }

    m_watching.store(false, std::memory_order_relaxed);
    m_authenticating.store(false, std::memory_order_relaxed);
    m_authorizing.store(false, std::memory_order_relaxed);
    m_events.wakeup();
    if (m_deliveryThread.joinable())
    {
        m_deliveryThread.join();
    }

    for (size_t i = 0; i < m_authenticationThreads.size(); ++i)
    {
        m_authentications.wakeup();
    }
    for (auto& thread: m_authenticationThreads)
    {
        if (thread.joinable())
        {
            // An authenticator that never returns holds its thread here. Nothing can be done about
            // that from this side - there is no way to cancel a call already inside somebody
            // else's library - which is why the pool is small and why the log says who it is.
            thread.join();
        }
    }
    m_authenticationThreads.clear();

    // Anything still queued is owed an answer, and a broker that is stopping cannot give a real
    // one. Refusing is the honest reply and matches what a caller does with Unavailable.
    PendingAuth pending;
    while (m_authentications.pop_front(pending, chrono::milliseconds(0)))
    {
        m_queuedAuthentications.fetch_sub(1, std::memory_order_relaxed);
        if (pending.m_answer)
        {
            pending.m_answer(AuthDecision::Unavailable, nullptr);
        }
    }

    if (const auto refused = m_refusedAuthentications.load(std::memory_order_relaxed); refused > 0)
    {
        m_logger.warning(format("{} connections were refused because no authentication thread was free",
                                refused));
    }

    if (const auto dropped = m_dropped.load(std::memory_order_relaxed); dropped > 0)
    {
        m_logger.warning(format("{} events were dropped: an observer could not keep up", dropped));
    }

    // Emptied first: the threads above have been joined by now, but the snapshot is what any
    // straggler would iterate, and it must not name records that are about to be destroyed.
    m_active.store(nullptr, std::memory_order_release);

    // Reverse order, so an extension that was loaded first is destroyed last - the same courtesy
    // the loader gives its own libraries.
    for (auto& extension: std::views::reverse(m_loaded))
    {
        unload(*extension);
    }
    m_loaded.clear();
}

void ExtensionHost::publishActiveUnlocked()
{
    auto active = std::make_unique<ActiveList>();
    auto watching = false;
    auto authenticating = false;
    auto authorizing = false;

    for (const auto& loaded: m_loaded)
    {
        if (!loaded->m_started)
        {
            continue;
        }
        active->push_back(loaded.get());

        if (loaded->m_table->observer != nullptr && loaded->m_table->observer->on_event != nullptr)
        {
            watching = true;
        }
        if (authenticatorOfTable(*loaded->m_table) != nullptr)
        {
            authenticating = true;
        }
        if (authorizerOfTable(*loaded->m_table) != nullptr)
        {
            authorizing = true;
        }
    }

    // Recomputed rather than only ever set: switching the last authenticator off has to put the
    // CONNECT path back to the shape it had before any extension was loaded, or the broker keeps
    // paying for a capability nobody provides.
    m_watching.store(watching, std::memory_order_relaxed);
    m_authenticating.store(authenticating, std::memory_order_relaxed);
    m_authorizing.store(authorizing, std::memory_order_relaxed);

    // Kept rather than swapped out: a reader may still be walking the previous one, and it holds a
    // bare pointer to it.
    m_snapshots.push_back(std::move(active));
    m_active.store(m_snapshots.back().get(), std::memory_order_release);
}

bool ExtensionHost::quiesce(Loaded& loaded, const Logger& logger)
{
    // Generous, because an authenticator is allowed to block on a directory server and a client is
    // waiting on that answer. Bounded, because a wait without an end is a broker that stops
    // answering its own interface.
    constexpr auto patience = chrono::seconds(30);
    constexpr auto step = chrono::milliseconds(20);

    const auto deadline = chrono::steady_clock::now() + patience;
    while (loaded.m_inFlight.load(std::memory_order_acquire) > 0)
    {
        if (chrono::steady_clock::now() >= deadline)
        {
            logger.error(loaded.m_name + ": " + to_string(loaded.m_inFlight.load()) +
                         " call(s) are still inside it after " + to_string(patience.count()) +
                         "s; leaving it running rather than pulling the ground from under them");
            return false;
        }
        this_thread::sleep_for(step);
    }
    return true;
}

vector<ExtensionHost::Description> ExtensionHost::describe() const
{
    vector<Description> described;

    const std::scoped_lock lock(m_loadedLock);
    for (const auto& loaded: m_loaded)
    {
        Description one;
        one.m_name = loaded->m_name;
        one.m_library = loaded->m_library;
        one.m_source = loaded->m_source;
        one.m_running = loaded->m_started;
        one.m_required = loaded->m_required;
        one.m_eventsDelivered = loaded->m_eventsDelivered.load(std::memory_order_relaxed);
        one.m_admitted = loaded->m_admitted.load(std::memory_order_relaxed);
        one.m_refused = loaded->m_refused.load(std::memory_order_relaxed);
        one.m_storeErrors = loaded->m_storeErrors.load(std::memory_order_relaxed);

        if (loaded->m_table != nullptr)
        {
            one.m_version = loaded->m_table->version == nullptr ? "" : loaded->m_table->version;
            one.m_capabilities = loaded->m_table->capabilities;

            one.m_description = loaded->m_table->description == nullptr ? "" : loaded->m_table->description;
        }

        const std::scoped_lock settingsLock(loaded->m_settingsLock);
        auto                   remaining = loaded->m_settings;

        if (loaded->m_table != nullptr && loaded->m_table->settings != nullptr)
        {
            for (size_t at = 0; at < loaded->m_table->setting_count; ++at)
            {
                const auto&        declared = loaded->m_table->settings[at];
                SettingDescription setting;
                setting.m_name = declared.name == nullptr ? "" : declared.name;
                setting.m_label = declared.label == nullptr ? setting.m_name : declared.label;
                setting.m_description = declared.description == nullptr ? "" : declared.description;
                setting.m_type = static_cast<xmq_setting_type>(declared.type);
                setting.m_defaultValue = declared.default_value == nullptr ? "" : declared.default_value;
                setting.m_choices = declared.choices == nullptr ? "" : declared.choices;
                setting.m_required = declared.required != 0;

                if (const auto held = remaining.find(setting.m_name); held != remaining.end())
                {
                    setting.m_value = held->second;
                    remaining.erase(held);
                }
                one.m_settings.push_back(std::move(setting));
            }
        }

        // Whatever is left is set but undeclared - usually a misspelled key, which is the commonest
        // reason a setting quietly does nothing.
        for (const auto& [name, value]: remaining)
        {
            one.m_settings.push_back({.m_name = name, .m_label = name, .m_value = value, .m_declared = false});
        }

        described.push_back(std::move(one));
    }

    // And the ones the configuration switched off. Their libraries were never opened, so what can
    // be said about them is what the file says: name, library, source, settings. Version and
    // description would have to come from inside the library, and opening it is exactly what being
    // switched off forbids.
    for (const auto& configured: m_configuredOnly)
    {
        Description one;
        one.m_name = configured.m_name;
        one.m_library = configured.m_library;
        one.m_source = configured.m_source;
        one.m_running = false;
        one.m_required = configured.m_required;

        for (const auto& [name, value]: configured.m_settings)
        {
            one.m_settings.push_back({.m_name = name, .m_label = name, .m_value = value,
                                      .m_declared = false});
        }
        described.push_back(std::move(one));
    }
    return described;
}

namespace {

/// Said in the log as well as returned. The answer reaches the screen, but a screen is free to
/// ignore it - and one did, which is how a refused write looked like a silent success.
ExtensionHost::Report refusedWrite(const Logger& logger, const string& name, const string& reason)
{
    const auto refusal = name + ": " + reason;
    logger.error(refusal);
    return ExtensionHost::Report::problem(refusal);
}

} // namespace

ExtensionHost::Report ExtensionHost::writeEnabled(const string& name, const bool enabled)
{
    filesystem::path source;
    {
        const std::scoped_lock lock(m_loadedLock);

        const auto loaded = ranges::find_if(m_loaded, [&name](const auto& candidate)
        {
            return candidate->m_name == name;
        });
        if (loaded != m_loaded.end())
        {
            source = (*loaded)->m_source;
        }
        else
        {
            const auto known = ranges::find_if(m_configuredOnly, [&name](const auto& candidate)
            {
                return candidate.m_name == name;
            });
            if (known == m_configuredOnly.end())
            {
                return refusedWrite(m_logger, name, "no such extension");
            }
            source = known->m_source;
        }
    }

    if (source.empty())
    {
        return refusedWrite(m_logger, name, "its configuration entry has no file to write back to");
    }

    try
    {
        Buffer content;
        content.loadFromFile(source);

        xdoc::Document document;
        document.load(content);

        auto found = false;
        for (const auto& node: document.root()->nodes("extensions"))
        {
            if (String(node->getString("name")).trim() != name)
            {
                continue;
            }
            node->set("enabled", enabled);
            found = true;
            break;
        }

        if (!found)
        {
            return refusedWrite(m_logger, name, source.string() + " no longer holds an entry for it");
        }

        Buffer written;
        document.exportTo(xdoc::DataFormat::JSON, written, true);
        written.saveToFile(source);
    }
    catch (const Exception& e)
    {
        return refusedWrite(m_logger, name, "could not write " + source.string() + ": " + e.what());
    }

    const auto written = name + (enabled ? ": enabled in " : ": disabled in ") + source.string();
    m_logger.info(written);
    return Report::note(written);
}

ExtensionHost::Report ExtensionHost::writeSettings(const string& name, const map<string, string>& settings)
{
    filesystem::path    source;
    map<string, string> merged = settings;

    {
        const std::scoped_lock lock(m_loadedLock);

        const auto loaded = ranges::find_if(m_loaded, [&name](const auto& candidate)
        {
            return candidate->m_name == name;
        });
        if (loaded == m_loaded.end())
        {
            return refusedWrite(m_logger, name, "no such extension");
        }
        source = (*loaded)->m_source;
        if (source.empty())
        {
            return refusedWrite(m_logger, name, "its configuration entry has no file to write back to");
        }

        // The mask the screen was shown is what the screen sends back for an untouched secret.
        // Writing it would replace a password with a row of asterisks.
        if ((*loaded)->m_table != nullptr && (*loaded)->m_table->settings != nullptr)
        {
            const std::scoped_lock settingsLock((*loaded)->m_settingsLock);
            for (size_t at = 0; at < (*loaded)->m_table->setting_count; ++at)
            {
                const auto& declared = (*loaded)->m_table->settings[at];
                if (declared.type != XMQ_SETTING_SECRET || declared.name == nullptr)
                {
                    continue;
                }
                const auto incoming = merged.find(declared.name);
                if (incoming == merged.end() || incoming->second != secretMask)
                {
                    continue;
                }
                if (const auto held = (*loaded)->m_settings.find(declared.name);
                    held != (*loaded)->m_settings.end())
                {
                    incoming->second = held->second;
                }
                else
                {
                    merged.erase(incoming);
                }
            }
        }
    }

    try
    {
        Buffer content;
        content.loadFromFile(source);

        xdoc::Document document;
        document.load(content);

        auto found = false;
        for (const auto& node: document.root()->nodes("extensions"))
        {
            if (String(node->getString("name")).trim() != name)
            {
                continue;
            }

            // Replaced wholesale rather than merged: a key the screen no longer sends is a key the
            // operator removed, and merging would make removal impossible.
            auto settingsNode = node->findFirst("settings", xdoc::SearchMode::Recursive);
            if (!settingsNode)
            {
                settingsNode = node->pushNode("settings");
            }
            settingsNode->clear();
            for (const auto& [key, value]: merged)
            {
                settingsNode->set(key, value);
            }
            found = true;
            break;
        }

        if (!found)
        {
            return refusedWrite(m_logger, name, source.string() + " no longer holds an entry for it");
        }

        Buffer written;
        document.exportTo(xdoc::DataFormat::JSON, written, true);
        written.saveToFile(source);

        // The same restriction the broker's own configuration gets: this file holds connection URIs
        // with passwords in them.
        error_code errorCode;
        filesystem::permissions(source,
                                filesystem::perms::owner_read | filesystem::perms::owner_write |
                                filesystem::perms::group_read,
                                filesystem::perm_options::replace, errorCode);
    }
    catch (const Exception& e)
    {
        return refusedWrite(m_logger, name, "could not write " + source.string() + ": " + e.what());
    }

    const auto written = name + ": settings written to " + source.string();
    m_logger.info(written);
    return Report::note(written);
}

ExtensionHost::Report ExtensionHost::enable(const Configured& configured)
{
    Loaded* found = nullptr;
    {
        const std::scoped_lock lock(m_loadedLock);
        for (const auto& candidate: m_loaded)
        {
            if (candidate->m_name == configured.m_name)
            {
                found = candidate.get();
                break;
            }
        }

        if (found != nullptr && found->m_started)
        {
            return Report::note(configured.m_name + ": already running");
        }

        if (found == nullptr)
        {
            // Switched off in the file until now, so its library has never been opened. It leaves
            // the not-loaded list here and joins the loaded one below.
            std::erase_if(m_configuredOnly, [&configured](const auto& candidate)
            {
                return candidate.m_name == configured.m_name;
            });

            // First time: the library has to be opened. load() also creates the instance.
            auto loaded = load(configured);
            if (!loaded)
            {
                if (const auto reason = takeLastError(*found); !reason.empty())
                {
                    return Report::problem(configured.m_name + ": could not be loaded.\n\n" + reason);
                }
                return Report::problem(configured.m_name + ": could not be loaded, see the log");
            }
            m_loaded.push_back(std::move(loaded));
            found = m_loaded.back().get();
        }
        else
        {
            // Switched off earlier: the library is still mapped, only the instance was destroyed.
            // The settings are taken again, because they may have been edited while it was off.
            {
                const std::scoped_lock settingsLock(found->m_settingsLock);
                found->m_settings = configured.m_settings;
            }

            found->m_instance = found->m_table->create(&found->m_hostTable);
            if (found->m_instance == nullptr)
            {
                return Report::problem(configured.m_name + ": could not be created again");
            }
        }

        if (found->m_table->start != nullptr && found->m_table->start(found->m_instance) != XMQ_OK)
        {
            // Destroyed rather than left half-alive: an instance whose start() failed has said it
            // cannot work, and keeping it would put it in the snapshot on the next attempt.
            found->m_table->destroy(found->m_instance);
            found->m_instance = nullptr;
            if (const auto reason = takeLastError(*found); !reason.empty())
            {
                return Report::problem(configured.m_name + ": refused to start.\n\n" + reason);
            }
            return Report::problem(configured.m_name + ": refused to start, see the log");
        }

        // Only now, and never before start(): an extension in the snapshot is one the broker may
        // ask anything of, at once, from another thread.
        found->m_started = true;
        publishActiveUnlocked();
    }

    // The threads the capabilities need may not exist yet - nothing declared them when the broker
    // started. Started here rather than in start(), which is for the initial load.
    startWorkerThreads();

    m_logger.info(configured.m_name + ": started");
    return Report::note(configured.m_name + ": started");
}

ExtensionHost::Report ExtensionHost::disable(const string& name)
{
    Loaded* found = nullptr;
    {
        const std::scoped_lock lock(m_loadedLock);
        for (const auto& candidate: m_loaded)
        {
            if (candidate->m_name == name)
            {
                found = candidate.get();
                break;
            }
        }

        if (found == nullptr || !found->m_started)
        {
            return Report::note(name + ": not running");
        }
        if (found->m_required)
        {
            // The broker does not serve MQTT without it, so this would be a way to stop serving
            // that does not say it is one.
            return Report::problem(name +
                                   ": required by the configuration - change that and restart if it should go");
        }

        // Out of the snapshot first: nothing new reaches it from here on, which is what makes the
        // wait below finite.
        found->m_started = false;
        publishActiveUnlocked();
    }

    if (!quiesce(*found, m_logger))
    {
        // Left off but not stopped. Its instance leaks until the broker restarts, which is the
        // recoverable half of the choice.
        // Switched off as asked, but quiescing timed out - abnormal, and the operator has to
        // be told rather than shown a plain success.
        return Report::problem(name + ": switched off, but calls are still inside it - not stopped");
    }

    if (found->m_table->stop != nullptr && found->m_table->stop(found->m_instance) != XMQ_OK)
    {
        m_logger.warning(name + ": stop() reported a failure");
    }
    found->m_table->destroy(found->m_instance);
    found->m_instance = nullptr;

    m_logger.info(name + ": stopped");
    return Report::note(name + ": stopped");
}

void ExtensionHost::unload(Loaded& loaded)
{
    if (loaded.m_started && loaded.m_table != nullptr && loaded.m_table->stop != nullptr)
    {
        loaded.m_table->stop(loaded.m_instance);
        loaded.m_started = false;
    }
    if (loaded.m_instance != nullptr && loaded.m_table != nullptr && loaded.m_table->destroy != nullptr)
    {
        loaded.m_table->destroy(loaded.m_instance);
        loaded.m_instance = nullptr;
    }
    if (loaded.m_handle != nullptr)
    {
        closeLibrary(loaded.m_handle);
        loaded.m_handle = nullptr;
    }
}

void ExtensionHost::publishEvent(const xmq_event_type  type, const string_view     clientId,
                                 const string_view     username, const string_view topic,
                                 const size_t          payloadSize, const uint8_t  qos, const bool retain,
                                 const EventAttributes attributes)
{
    if (!m_watching.load(std::memory_order_relaxed))
    {
        return;
    }

    // The subject this event belongs to may be switched off. One relaxed load, in front of the
    // three string copies below - which is the whole point on the publish path, where this runs
    // once per message.
    if ((m_enabledEvents.load(std::memory_order_relaxed) & subjectBit(subjectOf(type))) == 0)
    {
        return;
    }

    // The bound is the whole point. Every caller of this is on a thread that carries messages, and
    // an observer that stops consuming must cost events rather than memory or delivery.
    if (m_queued.load(std::memory_order_relaxed) >= MaxQueuedEvents)
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    QueuedEvent queued{.m_type = type,
                       .m_timestampUs = nowMicroseconds(),
                       .m_clientId = string(clientId),
                       .m_username = string(username),
                       .m_topic = string(topic),
                       .m_payloadSize = payloadSize,
                       .m_qos = qos,
                       .m_retain = retain};

    // One relaxed load when nobody asked for anything, which is the ordinary case and the one on
    // the publish path: an attribute offered here is not copied unless some extension wants it.
    if (const auto wanted = m_wantedAttributes.load(std::memory_order_relaxed);
        wanted != 0)
    {
        for (const auto& attribute: attributes)
        {
            if ((wanted & attributeBit(attribute.m_name)) != 0)
            {
                queued.m_attributes.emplace_back(attribute.m_name, attribute.m_value);
            }
        }
    }

    m_queued.fetch_add(1, std::memory_order_relaxed);
    m_events.push_back(std::move(queued));
}

void ExtensionHost::publishError(const LogSubject  subject, const string_view      reason,
                                 const string_view message, const string_view      clientId,
                                 const string_view username, const EventAttributes attributes)
{
    if (!m_watching.load(std::memory_order_relaxed) ||
        (m_enabledEvents.load(std::memory_order_relaxed) & subjectBit(subject)) == 0)
    {
        return;
    }

    if (m_queued.load(std::memory_order_relaxed) >= MaxQueuedEvents)
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    QueuedEvent queued{.m_type = XMQ_EVENT_ERROR,
                       .m_timestampUs = nowMicroseconds(),
                       .m_clientId = string(clientId),
                       .m_username = string(username)};

    // Taken whether or not anybody asked: these three are what the event says, not extra facts
    // about it, and an error an extension cannot identify is worse than no error at all.
    queued.m_attributes.emplace_back(errorSubjectAttribute, to_string(subject));
    queued.m_attributes.emplace_back(errorReasonAttribute, reason);
    queued.m_attributes.emplace_back(errorMessageAttribute, message);

    if (const auto wanted = m_wantedAttributes.load(std::memory_order_relaxed);
        wanted != 0)
    {
        for (const auto& attribute: attributes)
        {
            if ((wanted & attributeBit(attribute.m_name)) != 0)
            {
                queued.m_attributes.emplace_back(attribute.m_name, attribute.m_value);
            }
        }
    }

    m_queued.fetch_add(1, std::memory_order_relaxed);
    m_events.push_back(std::move(queued));
}

void ExtensionHost::authenticate(AuthRequest                                        request,
                                 function<void(AuthDecision, shared_ptr<AclGroup>)> answer)
{
    if (!m_authenticating.load(std::memory_order_relaxed) && !m_authorizing.load(std::memory_order_relaxed))
    {
        answer(AuthDecision::NotHandled, nullptr);
        return;
    }

    if (m_queuedAuthentications.load(std::memory_order_relaxed) >=
        m_maxQueuedAuthentications.load(std::memory_order_relaxed))
    {
        // Every authentication thread is inside somebody else's library and the queue behind them
        // is full. There is no answer to be had, and "we could not check" has exactly one safe
        // reading.
        m_refusedAuthentications.fetch_add(1, std::memory_order_relaxed);
        answer(AuthDecision::Unavailable, nullptr);
        return;
    }

    m_queuedAuthentications.fetch_add(1, std::memory_order_relaxed);
    m_authentications.push_back(PendingAuth{.m_request = std::move(request), .m_answer = std::move(answer)});
}

ExtensionHost::AuthDecision ExtensionHost::askAuthenticators(const AuthRequest& request)
{
    const xmq_auth_request wire{
        .client_id = {.data = request.m_clientId.c_str(), .length = request.m_clientId.size()},
        .username = {.data = request.m_username.c_str(), .length = request.m_username.size()},
        .password = {.data = request.m_password.c_str(), .length = request.m_password.size()},
        .remote_address = {.data = request.m_remoteAddress.c_str(), .length = request.m_remoteAddress.size()},
        .protocol_version = request.m_protocolVersion,
        .encrypted = static_cast<uint8_t>(request.m_encrypted ? 1 : 0),
        .reserved = {}};

    // Configuration order, and the first opinion wins: an operator who lists a directory before a
    // fallback means the directory to have the say.
    //
    // The snapshot, not the owning list: it holds exactly the extensions that are on, and it cannot
    // move under this loop while somebody switches one off.
    const auto* active = m_active.load(std::memory_order_acquire);
    if (active == nullptr)
    {
        return AuthDecision::NotHandled;
    }
    for (auto* loaded: *active)
    {
        const auto* authenticator = authenticatorOfTable(*loaded->m_table);
        if (authenticator == nullptr)
        {
            continue;
        }

        const InFlight inFlight(loaded->m_inFlight);
        switch (authenticator->authenticate(loaded->m_instance, &wire))
        {
            case XMQ_AUTH_ALLOW:
                loaded->m_admitted.fetch_add(1, std::memory_order_relaxed);
                return AuthDecision::Allow;
            case XMQ_AUTH_DENY:
                loaded->m_refused.fetch_add(1, std::memory_order_relaxed);
                m_logger.info(format("{} refused {}", loaded->m_name, request.m_clientId));
                return AuthDecision::Deny;
            case XMQ_AUTH_NOT_HANDLED:
                break;
            case XMQ_AUTH_SUBSYSTEM_ERROR:
                // Not passed to the next authenticator and not left to the broker's own accounts:
                // an extension that cannot read its store has not said "someone else's client",
                // it has said "I cannot tell", and treating that as an abstention would let an
                // outage admit whoever the fallback happens to know.
                loaded->m_storeErrors.fetch_add(1, std::memory_order_relaxed);
                m_logger.error(format("{} could not reach its authentication store; refusing {}",
                                      loaded->m_name, request.m_clientId));
                return AuthDecision::SubsystemError;
        }
    }
    return AuthDecision::NotHandled;
}

std::shared_ptr<AclGroup> ExtensionHost::resolveGroup(const AuthRequest& request, bool& refused)
{
    refused = false;
    const xmq_auth_request wire{
        .client_id = {.data = request.m_clientId.c_str(), .length = request.m_clientId.size()},
        .username = {.data = request.m_username.c_str(), .length = request.m_username.size()},
        .password = {.data = request.m_password.c_str(), .length = request.m_password.size()},
        .remote_address = {.data = request.m_remoteAddress.c_str(), .length = request.m_remoteAddress.size()},
        .protocol_version = request.m_protocolVersion,
        .encrypted = static_cast<uint8_t>(request.m_encrypted ? 1 : 0),
        .reserved = {}};

    string      groupName;
    const auto* active = m_active.load(std::memory_order_acquire);
    if (active == nullptr)
    {
        return {};
    }
    for (auto* loaded: *active)
    {
        const auto* authorizer = authorizerOfTable(*loaded->m_table);
        if (authorizer == nullptr || authorizer->resolve_group == nullptr)
        {
            continue;
        }

        array<char, MaxGroupNameLength> name{};
        const InFlight                  inFlight(loaded->m_inFlight);
        const auto                      length = authorizer->resolve_group(loaded->m_instance, &wire, name.data(), name.size());
        if (length >= name.size())
        {
            // Not cut to fit: two long names that share their beginning would become one group,
            // and each would get the other's rights.
            m_logger.error(format("{} named a {}-byte group for {}, longer than the {} bytes a group "
                                  "name may have; refusing the connection",
                                  loaded->m_name, length, request.m_clientId, name.size() - 1));
            refused = true;
            return {};
        }
        if (length > 0)
        {
            groupName.assign(name.data(), length);
            break;
        }
    }

    // Shared with everyone else in the group, which is the point. The unnamed group is a group
    // like any other, so a client nobody recognised is also answered from cache.
    {
        const shared_lock lock(m_groupsLock);
        if (const auto found = m_groups.find(groupName); found != m_groups.end())
        {
            return found->second;
        }
    }

    const scoped_lock lock(m_groupsLock);
    auto&             group = m_groups[groupName];
    if (!group)
    {
        group = make_shared<AclGroup>(this, groupName);
    }
    return group;
}

void ExtensionHost::invalidateAcl()
{
    const shared_lock lock(m_groupsLock);
    for (const auto& [name, group]: m_groups)
    {
        group->forget();
    }
    m_logger.info("cached permissions dropped; they will be asked for again");
}

AclDecision ExtensionHost::askAuthorizers(const string&        group, const string_view topic,
                                          const xmq_acl_action action)
{
    const xmq_str wireGroup{.data = group.c_str(), .length = group.size()};
    const xmq_str wireTopic{.data = topic.data(), .length = topic.size()};

    const auto* active = m_active.load(std::memory_order_acquire);
    if (active == nullptr)
    {
        return AclDecision::NotHandled;
    }
    for (auto* loaded: *active)
    {
        const auto* authorizer = authorizerOfTable(*loaded->m_table);
        if (authorizer == nullptr)
        {
            continue;
        }

        const InFlight inFlight(loaded->m_inFlight);
        switch (authorizer->authorize(loaded->m_instance, &wireGroup, &wireTopic, action))
        {
            case XMQ_ACL_ALLOW:
                return AclDecision::Allow;
            case XMQ_ACL_DENY:
                return AclDecision::Deny;
            case XMQ_ACL_NOT_HANDLED:
                break;
            case XMQ_ACL_SUBSYSTEM_ERROR:
                // Debug, not error: this answer is deliberately not cached, so an outage would
                // otherwise write a line for every message the group publishes. Something
                // rate-limited would be better if this ever needs to be visible by default.
                m_logger.debug([&loaded]
                {
                    return loaded->m_name + " could not reach its permission store";
                });
                return AclDecision::SubsystemError;
        }
    }
    return AclDecision::NotHandled;
}

namespace {

/// One remembered answer, private to the thread that wrote it.
struct LocalDecision
{
    /// The group's own identity, not its address: see AclGroup::m_id for why an address will not do.
    uint64_t       m_groupId{0};
    uint64_t       m_generation{0};
    xmq_acl_action m_action{};
    AclDecision    m_decision{};
    string         m_topic;
};

/**
 * @brief A small direct-mapped cache in front of the shared one, per thread.
 *
 * The shared map is guarded by a shared_mutex, and taking a read lock is still a write: every
 * reader modifies the reader count, so the cacheline holding it bounces between cores. Measured on
 * a warm cache, that turned 17ns on one thread into 104ns aggregate on four - a check that got
 * slower the more of the broker was working, which is the wrong direction for anything on a
 * message path.
 *
 * A hit here writes nothing anywhere shared. It costs a hash, an equality test on a short string,
 * and a load of the group's generation counter, which is read-only until the rules change.
 *
 * Direct-mapped rather than associative because a miss is not a failure - it falls through to the
 * shared map, which is correct and merely slower. Sizing it is therefore a tuning choice and never
 * a correctness one.
 */
constexpr size_t                                  LocalDecisions = 256;
thread_local array<LocalDecision, LocalDecisions> localDecisions;

} // namespace


uint64_t AclGroup::nextId()
{
    // Starts at 1 so that a zero-initialised cache entry - which is what an untouched slot holds -
    // matches no group at all.
    static atomic<uint64_t> lastId{0};
    return lastId.fetch_add(1, memory_order_relaxed) + 1;
}

namespace {

size_t localSlot(const AclGroup* group, const string_view topic, const xmq_acl_action action)
{
    const auto mixed = hash<string_view>{}(topic) ^ (reinterpret_cast<uintptr_t>(group) >> 4U) ^
                       static_cast<size_t>(action) * 0x9E3779B9U;
    return mixed % LocalDecisions;
}

} // namespace

AclDecision AclGroup::authorize(const string_view topic, const xmq_acl_action action)
{
    const auto generation = m_generation.load(std::memory_order_relaxed);

    // The common case, and the only one that happens at message rate: this thread has already
    // asked about this topic for this group.
    auto& local = localDecisions[localSlot(this, topic, action)];
    if (local.m_groupId == m_id && local.m_generation == generation && local.m_action == action &&
        local.m_topic == topic)
    {
        return local.m_decision;
    }

    auto& decisions = action == XMQ_ACL_PUBLISH ? m_publish : m_subscribe;

    {
        // Not in this thread's own cache, but another thread may already have paid for the answer.
        const shared_lock lock(m_lock);
        if (const auto found = decisions.find(topic); found != decisions.end())
        {
            local = {.m_groupId = m_id,
                     .m_generation = generation,
                     .m_action = action,
                     .m_decision = found->second,
                     .m_topic = string(topic)};
            return found->second;
        }
    }

    // Asked outside the lock: authorize() is not allowed to block, but it is somebody else's code
    // and holding a lock across it would make a mistake there a broker-wide one. Two threads may
    // both ask about the same topic on the same first message; they get the same answer.
    const auto decision = m_host->askAuthorizers(m_name, topic, action);

    const scoped_lock lock(m_lock);

    // The rules may have changed while that call was outstanding. The answer still stands for this
    // operation - it is what the rules said when it was asked - but caching it now would put a
    // decision taken under the old rules into a map that forget() has just emptied, where it would
    // outlive the change and be served to every session in the group until the next one. That is
    // an access that was revoked and went on working. forget() bumps the generation under this
    // same lock, so comparing it here is enough to tell.
    if (m_generation.load(std::memory_order_relaxed) != generation)
    {
        return decision;
    }

    // A failure to reach the store says nothing about the rules, so there is nothing to remember.
    // Caching it would turn an outage of a second into a refusal lasting until the rules next
    // change, for every session in the group.
    if (decision == AclDecision::SubsystemError)
    {
        return decision;
    }

    decisions.emplace(topic, decision);
    local = {.m_groupId = m_id,
             .m_generation = generation,
             .m_action = action,
             .m_decision = decision,
             .m_topic = string(topic)};
    return decision;
}

void AclGroup::forget()
{
    const scoped_lock lock(m_lock);
    m_publish.clear();
    m_subscribe.clear();

    // Every thread's own copies are stale from here on. They are not reachable from this thread,
    // and they do not have to be: each carries the generation it was written under, and a
    // mismatch reads as a miss.
    m_generation.fetch_add(1, std::memory_order_relaxed);
}

void ExtensionHost::authenticationThread()
{
    constexpr auto waitTime = chrono::milliseconds(200);

    while (!m_terminated.load(std::memory_order_relaxed))
    {
        PendingAuth pending;
        if (!m_authentications.pop_front(pending, waitTime))
        {
            continue;
        }
        m_queuedAuthentications.fetch_sub(1, std::memory_order_relaxed);

        // Waited too long to be worth doing. The client has either given up or is about to, and the
        // work would be a database round trip and a KDF spent on an answer nobody reads - while the
        // requests behind it wait for this thread. This is what bounds the damage a hanging store
        // does, now that the queue is deep enough not to bound it by refusing legitimate bursts.
        if (std::chrono::steady_clock::now() - pending.m_queuedAt > MaxQueueWait)
        {
            m_refusedAuthentications.fetch_add(1, std::memory_order_relaxed);
            if (pending.m_answer)
            {
                pending.m_answer(AuthDecision::Unavailable, nullptr);
            }
            continue;
        }

        auto decision = m_authenticating.load(std::memory_order_relaxed)
                            ? askAuthenticators(pending.m_request)
                            : AuthDecision::NotHandled;

        // Only for a client that is getting in. Asking a directory which groups a refused client
        // belongs to is work nobody will use, and on the connect path of a client being refused.
        shared_ptr<AclGroup> group;
        if (decision != AuthDecision::Deny && decision != AuthDecision::Unavailable &&
            m_authorizing.load(std::memory_order_relaxed))
        {
            bool refused = false;
            group = resolveGroup(pending.m_request, refused);
            if (refused)
            {
                decision = AuthDecision::Deny;
            }
        }

        if (pending.m_answer)
        {
            // Called on this thread rather than handed back to a receive worker: what follows a
            // decision is session setup and a CONNACK, neither of which is on a message path, and
            // bouncing it would buy a hop and lose the simplicity.
            pending.m_answer(decision, std::move(group));
        }
    }
}

void ExtensionHost::deliveryThread()
{
    constexpr auto      waitTime = chrono::milliseconds(200);
    constexpr size_t    maxRound = 64;
    vector<QueuedEvent> round;

    while (!m_terminated.load(std::memory_order_relaxed))
    {
        if (!m_events.pop_front(round, maxRound, waitTime))
        {
            continue;
        }
        m_queued.fetch_sub(round.size(), std::memory_order_relaxed);

        for (const auto& queued: round)
        {
            const xmq_event event{
                .type = (uint32_t) queued.m_type,
                .timestamp_us = queued.m_timestampUs,
                .client_id = {.data = queued.m_clientId.c_str(), .length = queued.m_clientId.size()},
                .username = {.data = queued.m_username.c_str(), .length = queued.m_username.size()},
                .topic = {.data = queued.m_topic.c_str(), .length = queued.m_topic.size()},
                .payload_size = queued.m_payloadSize,
                .qos = queued.m_qos,
                .retain = static_cast<uint8_t>(queued.m_retain ? 1 : 0),
                .reserved = {}};

            // Named for the extension's event_attribute(), which arrives on this very thread and
            // is answered only for the event in hand.
            m_deliveringEvent = &event;
            m_deliveringAttributes = &queued.m_attributes;

            if (const auto* active = m_active.load(std::memory_order_acquire);
                active != nullptr)
            {
                for (auto* loaded: *active)
                {
                    if (loaded->m_table->observer != nullptr &&
                        loaded->m_table->observer->on_event != nullptr)
                    {
                        const InFlight inFlight(loaded->m_inFlight);
                        loaded->m_table->observer->on_event(loaded->m_instance, &event);
                        loaded->m_eventsDelivered.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            // Cleared with the event that is going out of scope: an extension that squirrelled the
            // pointer away and asks later must get nothing rather than the next event's answers.
            m_deliveringEvent = nullptr;
            m_deliveringAttributes = nullptr;
        }
    }
}