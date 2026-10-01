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

/**
 * @file XmqExtension.h
 * @brief Writing an extension in C++ without letting C++ near the boundary.
 *
 * Header-only, and deliberately thin: it is the same ABI, with the C bookkeeping done once here
 * instead of in every extension. Derive from XmqExtensionBase, implement what you need, and
 * declare it with XMQ_DEFINE_EXTENSION.
 *
 *     class EventLog : public xmq::XmqExtensionBase
 *     {
 *     public:
 *         using XmqExtensionBase::XmqExtensionBase;
 *         void onEvent(const xmq_event& event) override { ... }
 *     };
 *
 *     XMQ_DEFINE_EXTENSION(EventLog, "event-log", "1.0")
 *
 * The macro catches every exception on the way out. An exception crossing into the broker would
 * unwind through C, which is undefined behaviour and in practice a crash of the whole broker
 * rather than of the extension that caused it.
 */

#pragma once

#include "xmq_extension.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

namespace xmq {

/**
 * @brief What an extension inherits.
 *
 * The broker's services arrive in the constructor; everything else is a virtual to override. The
 * defaults do nothing and succeed, so an extension implements only what it is for.
 */
class XmqExtensionBase
{
public:
    explicit XmqExtensionBase(const xmq_host& host)
        : m_host(host)
    {
    }

    virtual ~XmqExtensionBase() = default;

    XmqExtensionBase(const XmqExtensionBase&) = delete;
    XmqExtensionBase& operator=(const XmqExtensionBase&) = delete;

    /// Begin work. Start threads here, not in the constructor: the broker may create an extension
    /// and then decline to start it.
    virtual bool start()
    {
        return true;
    }

    /// Stop and join everything start() began. Nothing is called after this returns.
    virtual bool stop()
    {
        return true;
    }

    /**
     * @brief The settings changed; read them again with setting().
     *
     * Called on a running broker, so that a long-lived installation need not be restarted to change
     * a setting, and everything it holds open stays open - that is what makes this different from
     * being stopped and started. The other calls keep running meanwhile, on their own threads, and
     * read the settings this replaces: swap them atomically, or guard them.
     *
     * Returning false leaves it running with what it had. An extension that cannot apply a change
     * says so rather than half-applying it: half a configuration is the state nobody can reason
     * about.
     *
     * The default declines, which is the honest answer for an extension that has not thought about
     * it: its settings then change only when the broker restarts.
     */
    virtual bool reload()
    {
        return false;
    }

    /**
     * @brief The accounts in the broker's user database changed; forget anything cached about them.
     *
     * Called after the interface saves an account or a group membership, from the thread that made
     * the change - so this is the cheap half of reload(), not another way to spell it: drop what
     * was cached and answer from the store again.
     *
     * The default does nothing, which is right for an extension that caches nothing or that
     * authenticates against something the broker does not edit.
     */
    virtual void accountsChanged()
    {
    }

    /// Something happened in the broker. Never called on a thread that carries messages, so this
    /// may take its time - but a slow observer means dropped events, not a slow broker.
    virtual void onEvent(const xmq_event& /*event*/)
    {
    }

    /// May this client connect? Called on a thread the broker keeps for authentication, so a
    /// blocking directory lookup here is not merely allowed but expected. Answer NOT_HANDLED for
    /// clients this extension was not meant to judge - denying them would lock out everyone the
    /// broker's own accounts or another extension would have admitted.
    virtual xmq_auth_decision authenticate(const xmq_auth_request& /*request*/)
    {
        return XMQ_AUTH_NOT_HANDLED;
    }

    /// Which group does this client belong to? Called once per connection, on the authentication
    /// thread, so a directory lookup here is expected. The name returned is this extension's own -
    /// the broker keeps it on the session and passes it back to authorize(), never reading it.
    virtual std::string resolveGroup(const xmq_auth_request& /*request*/)
    {
        return {};
    }

    /// May this group publish to, or subscribe to, this topic? Asked once per group and topic and
    /// then cached, so it is called rarely - but it is called on a thread that carries messages,
    /// so it must not block. Read a table prepared in start() and return.
    virtual xmq_acl_decision authorize(std::string_view /*group*/, std::string_view /*topic*/,
                                       xmq_acl_action /*action*/)
    {
        return XMQ_ACL_NOT_HANDLED;
    }

protected:
    void log(const xmq_log_priority priority, const std::string& message) const
    {
        if (m_host.log != nullptr)
        {
            m_host.log(m_host.context, priority, message.c_str());
        }
    }

    /// A value from this extension's own configuration block, or the fallback when absent.
    [[nodiscard]] std::string setting(const char* key, const std::string& fallback = {}) const
    {
        if (m_host.setting == nullptr)
        {
            return fallback;
        }
        std::string value;
        return copied([this, key](char* buffer, size_t size) { return m_host.setting(m_host.context, key, buffer, size); },
                      value)
                   ? value
                   : fallback;
    }

    /**
     * @brief Where the broker keeps its own accounts, as a database URI.
     *
     * For an authenticator that answers out of those accounts - the ones the Users screen edits.
     * Empty on a broker that keeps them elsewhere.
     *
     * Not a setting of this extension's own, deliberately: the interface writes to the broker's
     * address, and an extension carrying a second copy of it can be pointed somewhere else and
     * then admit nobody the interface has added, with neither side in a position to notice.
     */
    [[nodiscard]] std::string userDatabaseUri() const
    {
        if (m_host.user_database_uri == nullptr)
        {
            return {};
        }
        std::string value;
        copied([this](char* buffer, size_t size) { return m_host.user_database_uri(m_host.context, buffer, size); },
               value);
        return value;
    }

    [[nodiscard]] std::string brokerVersion() const
    {
        if (m_host.broker_version == nullptr)
        {
            return {};
        }
        const auto value = m_host.broker_version(m_host.context);
        return value.data == nullptr ? std::string {} : std::string(value.data, value.length);
    }

    /**
     * @brief Ask for facts about events that are not fields of xmq_event.
     *
     * Call it from start(). Values are captured as the event is queued, so a name asked for later
     * cannot reach back for what was never taken - and only what is asked for is captured, so a
     * name nobody wants costs the broker nothing.
     *
     * @param names  Comma-separated, e.g. "remote_address, disconnect_reason".
     * @return false when the broker is too old to offer this at all.
     */
    bool wantEventAttributes(const char* names) const
    {
        if (m_host.want_event_attributes == nullptr)
        {
            return false;
        }
        m_host.want_event_attributes(m_host.context, names);
        return true;
    }

    /**
     * @brief One of those facts, for the event being delivered.
     *
     * Valid only inside onEvent() and only for the event it was handed. Empty when the name was not
     * asked for, does not apply to this event, or is one this broker does not publish - so a
     * missing fact reads the same as a fact that is not there, and neither is an error.
     */
    [[nodiscard]] std::string_view eventAttribute(const xmq_event& event, const char* name) const
    {
        if (m_host.event_attribute == nullptr)
        {
            return {};
        }
        const auto value = m_host.event_attribute(m_host.context, &event, name);
        return value.data == nullptr ? std::string_view {} : std::string_view(value.data, value.length);
    }

    /// Drops what the broker has cached from authorize(), so that changed rules take effect on
    /// clients that are already connected.
    void invalidateAcl() const
    {
        if (m_host.invalidate_acl != nullptr)
        {
            m_host.invalidate_acl(m_host.context);
        }
    }

private:
    /**
     * @brief Read a string copied out by the host, retrying with a larger buffer if needed.
     * @param read     Host function that copies the string.
     * @param value    Receives the complete string when one is available.
     * @return False when the host has no value.
     */
    template<typename Read>
    static bool copied(const Read& read, std::string& value)
    {
        std::string buffer(256, '\0');
        auto        length = read(buffer.data(), buffer.size());
        if (length >= 0 && static_cast<size_t>(length) >= buffer.size())
        {
            buffer.assign(static_cast<size_t>(length) + 1, '\0');
            length = read(buffer.data(), buffer.size());
        }
        if (length < 0)
        {
            return false;
        }
        buffer.resize(std::min(static_cast<size_t>(length), buffer.size() - 1));
        value = std::move(buffer);
        return true;
    }

    xmq_host m_host;
};

/// A field of an event as a string_view. Empty when the field does not apply to the event.
inline std::string_view view(const xmq_str& text)
{
    return text.data == nullptr ? std::string_view {} : std::string_view(text.data, text.length);
}

/**
 * @brief Get the event type as an enum from its fixed-width representation.
 * @param event    Event to inspect.
 * @return Event type.
 */
inline xmq_event_type eventType(const xmq_event& event)
{
    return static_cast<xmq_event_type>(event.type);
}

} // namespace xmq

/**
 * @brief Define the exported symbol for an extension class.
 *
 * Everything the ABI needs, generated once: the version check, the C entry points, and the
 * exception barrier. Put it at namespace scope in exactly one translation unit.
 */
/**
 * @brief Define an extension that describes itself and its settings.
 *
 * @param ExtensionDescription  One line saying what it is for, shown wherever it is listed.
 * @param ExtensionSettings     A `static const xmq_setting[]` at namespace scope. Its size is taken
 *                              from the array, so it cannot fall out of step with a count.
 *
 * Declaring settings is what turns a bare key/value box in the interface into a form with labels,
 * types and defaults - and what lets a wrong value be refused before it reaches the extension
 * rather than at its next start.
 */
#define XMQ_DEFINE_EXTENSION_DESCRIBED(ExtensionClass, ExtensionName, ExtensionVersion,             \
                                       ExtensionCapabilities, ExtensionDescription, ExtensionSettings) \
    XMQ_DEFINE_EXTENSION_IMPL(ExtensionClass, ExtensionName, ExtensionVersion, ExtensionCapabilities, \
                              ExtensionDescription, (ExtensionSettings),                            \
                              (sizeof(ExtensionSettings) / sizeof((ExtensionSettings)[0])))

/// The same, for an extension that says nothing about itself beyond its name and version.
#define XMQ_DEFINE_EXTENSION(ExtensionClass, ExtensionName, ExtensionVersion, ExtensionCapabilities) \
    XMQ_DEFINE_EXTENSION_IMPL(ExtensionClass, ExtensionName, ExtensionVersion, ExtensionCapabilities, \
                              nullptr, nullptr, 0)

#define XMQ_DEFINE_EXTENSION_IMPL(ExtensionClass, ExtensionName, ExtensionVersion,                  \
                                  ExtensionCapabilities, ExtensionDescription, ExtensionSettings,   \
                                  ExtensionSettingCount)                                            \
    namespace {                                                                                     \
    void* xmqExtensionCreate(const xmq_host* host)                                                  \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return new ExtensionClass(*host);                                                       \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            return nullptr;                                                                         \
        }                                                                                           \
    }                                                                                               \
    void xmqExtensionDestroy(void* instance)                                                        \
    {                                                                                               \
        delete static_cast<ExtensionClass*>(instance);                                              \
    }                                                                                               \
    xmq_status xmqExtensionStart(void* instance)                                                    \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return static_cast<ExtensionClass*>(instance)->start() ? XMQ_OK : XMQ_ERROR;            \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            return XMQ_ERROR;                                                                       \
        }                                                                                           \
    }                                                                                               \
    xmq_status xmqExtensionStop(void* instance)                                                     \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return static_cast<ExtensionClass*>(instance)->stop() ? XMQ_OK : XMQ_ERROR;             \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            return XMQ_ERROR;                                                                       \
        }                                                                                           \
    }                                                                                               \
    xmq_status xmqExtensionReload(void* instance)                                                   \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return static_cast<ExtensionClass*>(instance)->reload() ? XMQ_OK : XMQ_ERROR;           \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            return XMQ_ERROR;                                                                       \
        }                                                                                           \
    }                                                                                               \
    void xmqExtensionAccountsChanged(void* instance)                                                \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            static_cast<ExtensionClass*>(instance)->accountsChanged();                              \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            /* Nothing to report it to: the call returns nothing and the broker has already made */ \
            /* the change. Swallowed rather than let out through a C boundary. */                   \
        }                                                                                           \
    }                                                                                               \
    void xmqExtensionOnEvent(void* instance, const xmq_event* event)                                \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            static_cast<ExtensionClass*>(instance)->onEvent(*event);                                \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
        }                                                                                           \
    }                                                                                               \
    xmq_auth_decision xmqExtensionAuthenticate(void* instance, const xmq_auth_request* request)     \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return static_cast<ExtensionClass*>(instance)->authenticate(*request);                  \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            /* An authenticator that threw has not allowed anyone. Denying is the only safe */      \
            /* reading of it, and the broker logs the refusal. */                                   \
            return XMQ_AUTH_DENY;                                                                   \
        }                                                                                           \
    }                                                                                               \
    size_t xmqExtensionResolveGroup(void* instance, const xmq_auth_request* request, char* group,   \
                                    size_t groupSize)                                               \
    {                                                                                               \
        if (groupSize > 0)                                                                          \
        {                                                                                           \
            group[0] = '\0';                                                                        \
        }                                                                                           \
        try                                                                                         \
        {                                                                                           \
            const auto name = static_cast<ExtensionClass*>(instance)->resolveGroup(*request);       \
            /* A name that does not fit is reported at its full length, never cut to fit: the */    \
            /* broker then refuses the client rather than put it in a group it does not belong to. */ \
            if (name.size() < groupSize)                                                            \
            {                                                                                       \
                std::memcpy(group, name.data(), name.size() + 1);                                   \
            }                                                                                       \
            return name.size();                                                                     \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            /* No group, which is not the same as a group with no rights: authorize() is still */   \
            /* asked, and answers for a client whose group could not be established. */             \
            return 0;                                                                               \
        }                                                                                           \
    }                                                                                               \
    xmq_acl_decision xmqExtensionAuthorize(void* instance, const xmq_str* group,                    \
                                           const xmq_str* topic, xmq_acl_action action)             \
    {                                                                                               \
        try                                                                                         \
        {                                                                                           \
            return static_cast<ExtensionClass*>(instance)->authorize(xmq::view(*group),             \
                                                                     xmq::view(*topic), action);    \
        }                                                                                           \
        catch (...)                                                                                 \
        {                                                                                           \
            /* Same reading as a throwing authenticator: it has not permitted anything. */          \
            return XMQ_ACL_DENY;                                                                    \
        }                                                                                           \
    }                                                                                               \
    constexpr xmq_observer      xmqExtensionObserver {.on_event = xmqExtensionOnEvent};             \
    constexpr xmq_authenticator xmqExtensionAuthenticator {.authenticate = xmqExtensionAuthenticate}; \
    constexpr xmq_authorizer    xmqExtensionAuthorizer {.resolve_group = xmqExtensionResolveGroup,  \
                                                        .authorize = xmqExtensionAuthorize};        \
    constexpr xmq_extension xmqExtensionTable {                                                     \
        .struct_size = sizeof(xmq_extension),                                                       \
        .name = ExtensionName,                                                                      \
        .version = ExtensionVersion,                                                                \
        .description = (ExtensionDescription),                                                      \
        .create = xmqExtensionCreate,                                                               \
        .destroy = xmqExtensionDestroy,                                                             \
        .start = xmqExtensionStart,                                                                 \
        .stop = xmqExtensionStop,                                                                   \
        .reload = xmqExtensionReload,                                                               \
        .accounts_changed = xmqExtensionAccountsChanged,                                            \
        .capabilities = (ExtensionCapabilities),                                                    \
        .observer = ((ExtensionCapabilities) &XMQ_CAP_OBSERVER) != 0 ? &xmqExtensionObserver : nullptr, \
        .authenticator =                                                                            \
            ((ExtensionCapabilities) &XMQ_CAP_AUTHENTICATOR) != 0 ? &xmqExtensionAuthenticator      \
                                                                  : nullptr,                        \
        .authorizer =                                                                               \
            ((ExtensionCapabilities) &XMQ_CAP_AUTHORIZER) != 0 ? &xmqExtensionAuthorizer            \
                                                               : nullptr,                           \
        .settings = (ExtensionSettings),                                                            \
        .setting_count = (ExtensionSettingCount)};                                                  \
    }                                                                                               \
    extern "C" XMQ_EXTENSION_EXPORT const xmq_extension* xmq_extension_describe(uint32_t abiMajor,   \
                                                                               uint32_t abiMinor)   \
    {                                                                                               \
        /* The broker is older than the header this was built against, and may lack something */    \
        /* this extension expects. Declining is the whole point of asking. */                       \
        if (abiMajor != XMQ_EXTENSION_ABI_MAJOR || abiMinor < XMQ_EXTENSION_ABI_MINOR)              \
        {                                                                                           \
            return nullptr;                                                                         \
        }                                                                                           \
        return &xmqExtensionTable;                                                                  \
    }
