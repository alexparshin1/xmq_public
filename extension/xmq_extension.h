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
 * @file xmq_extension.h
 * @brief The binary interface between the XMQ broker and an extension.
 *
 * This is deliberately C. An extension is built by someone else, with another compiler, against
 * another standard library, and loaded into the broker's process; a C++ interface across that line
 * breaks silently when any of those differ. C++ is for writing the extension - see
 * XmqExtension.h, which wraps all of this - but the line itself carries nothing but plain
 * structures and function pointers.
 *
 * Rules that hold for every call in both directions:
 *
 *   - No exception may cross. Report failure by return value.
 *   - Pointers handed to an extension are owned by the broker and valid only for the duration of
 *     the call. Copy anything you keep.
 *   - Strings carry an explicit length and are not guaranteed to be NUL-terminated; payloads are
 *     binary and frequently are not.
 *
 * Threads. The broker calls an extension from several threads at once, and an extension has to be
 * written for that:
 *
 *   - create(), start(), stop() and destroy() are never concurrent with anything: create() and
 *     start() come before any other call, and stop() only once every other call has returned.
 *   - Everything else may run concurrently, with itself and with each other - authenticate() and
 *     resolve_group() on each of the broker's authentication threads, authorize() on its message
 *     threads, on_event() on its event thread, reload() and accounts_changed() on the thread of
 *     whoever changed the configuration. Settings a reload() replaces are being read by the other
 *     calls while it runs; swap them atomically, or guard them.
 *   - Every xmq_host function may be called from any thread, at any time between create() and
 *     destroy().
 *
 * An extension is a shared library exporting exactly one symbol, xmq_extension_describe(). The
 * broker calls it, checks the ABI version, and then uses the returned table.
 */

#ifndef XMQ_EXTENSION_H
#define XMQ_EXTENSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Marks the one symbol an extension must export.
 *
 * Extensions are normally built with hidden visibility - everything an extension does not export
 * is one less symbol that can collide with the broker's or another extension's. That default also
 * hides the entry point unless it is marked, and a library whose entry point is hidden loads and
 * then fails with "exports no xmq_extension_describe", which reads like the wrong library rather
 * than the wrong build flags. Hence this.
 */
#if defined(_WIN32)
#define XMQ_EXTENSION_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XMQ_EXTENSION_EXPORT __attribute__((visibility("default")))
#else
#define XMQ_EXTENSION_EXPORT
#endif

/**
 * @brief ABI version, as major and minor.
 *
 * The broker loads an extension when the major versions match and the extension's minor is not
 * greater than its own: a newer broker runs an older extension, because everything added since is
 * additive and the extension simply does not use it. A newer extension against an older broker is
 * refused, because it may expect what is not there.
 *
 * Major changes when anything already published changes meaning, layout or order. That has a cost
 * measured in customer upgrades, so it should happen approximately never.
 */
/*
 * 1.0 is the ABI of XMQ 1.0, and where the promise begins. Everything numbered before it was
 * unpublished and is not honoured: an extension built against a pre-1.0 header is refused.
 *
 * From here on, a minor version only ever appends fields to the end of xmq_host and xmq_extension,
 * both of which carry their own size - so each side can tell how much of the other's table exists.
 */
#define XMQ_EXTENSION_ABI_MAJOR 1
#define XMQ_EXTENSION_ABI_MINOR 0

/** @brief Outcome of a call into an extension. */
typedef enum xmq_status
{
    XMQ_OK = 0,
    XMQ_ERROR = 1,             /**< The extension failed; it reports why through the log. */
    XMQ_NOT_SUPPORTED = 2      /**< The extension does not implement this, and the broker carries on. */
} xmq_status;

/** @brief Severity of a message an extension writes to the broker's log. */
typedef enum xmq_log_priority
{
    XMQ_LOG_DEBUG = 0,
    XMQ_LOG_INFO = 1,
    XMQ_LOG_WARNING = 2,
    XMQ_LOG_ERROR = 3
} xmq_log_priority;

/** @brief A string that is not required to be NUL-terminated. */
typedef struct xmq_str
{
    const char* data;          /**< Bytes; NULL when absent, which is not the same as empty. */
    size_t      length;
} xmq_str;

/** @brief What happened in the broker. */
typedef enum xmq_event_type
{
    XMQ_EVENT_CLIENT_CONNECTED = 0,
    XMQ_EVENT_CLIENT_DISCONNECTED = 1,
    XMQ_EVENT_SUBSCRIBED = 2,
    XMQ_EVENT_UNSUBSCRIBED = 3,
    XMQ_EVENT_PUBLISHED = 4,      /**< A message was accepted from a client, before delivery. */

    /**
     * @brief Something the broker refused or could not do.
     *
     * One type rather than one per kind of failure, so that the broker learning to report a new one
     * costs a value rather than an ABI version. Which failure it was arrives in the attributes,
     * always present on this event and never needing to be asked for:
     *
     *   error_subject  the part of the broker it happened in - "connect", "publish", "storage" -
     *                  and also the switch that governs it: an error is delivered only when its
     *                  subject's events are.
     *   error_reason   a stable token for code to match on: "anonymous_refused", "credentials",
     *                  "no_account", "disabled", "invalid_topic"...
     *   error_message  the same sentence that goes to the broker's log, for a person.
     *
     * The reason is deliberately finer than what the client is told. A client that fails to
     * authenticate learns only that it failed; an extension counting failures needs to tell a
     * sweep of invented usernames from a password being guessed at one account, because those are
     * different attacks. Nothing here may be relayed to the client that caused it.
     */
    XMQ_EVENT_ERROR = 5
} xmq_event_type;

/**
 * @brief One thing that happened, as delivered to an observer.
 *
 * Fields that do not apply to the event carry a NULL xmq_str: a disconnect has no topic. The
 * payload is not included - an observer that needed message bodies would be reading the whole
 * traffic of the broker through a queue, which is not what this is for.
 */
typedef struct xmq_event
{
    uint32_t       type;           /**< An xmq_event_type. Fixed width: the size of an enum is the
                                        compiler's choice, and this crosses between compilers. */
    uint64_t       timestamp_us;   /**< Wall clock, microseconds since the epoch. */
    xmq_str        client_id;
    xmq_str        username;       /**< Empty for an anonymous client. */
    xmq_str        topic;          /**< Topic for a publish, filter for a subscribe. */
    size_t         payload_size;   /**< Bytes of the message body, for a publish. */
    uint8_t        qos;
    uint8_t        retain;
    uint8_t        reserved[6];    /**< Zero-filled; keeps the struct's alignment explicit. */
} xmq_event;

/**
 * @brief What the broker offers an extension.
 *
 * Handed over at creation and valid for the extension's whole life. Every function may be called
 * from any thread the extension owns.
 */
typedef struct xmq_host
{
    size_t struct_size;            /**< sizeof(xmq_host) as the broker compiled it: a function
                                        appended in a later minor version exists only when this
                                        reaches past it. */
    void*  context;                /**< Opaque broker state; pass it back unchanged. */

    /** @brief Write to the broker's log, under the extension's name. */
    void (*log)(void* context, xmq_log_priority priority, const char* message);

    /**
     * @brief Read a setting from this extension's own configuration block.
     *
     * Copied into the caller's buffer, so that two threads asking at once cannot overwrite each
     * other's answer, and so that a reload() running meanwhile cannot change it underneath.
     *
     * @param buffer       Receives the value, NUL-terminated and cut to fit when it is too long.
     * @param buffer_size  Size of buffer, including room for the NUL.
     * @return The length of the whole value without the NUL - when it is buffer_size or more, the
     *         value did not fit: ask again with a larger buffer - or -1 when the key is absent.
     */
    int64_t (*setting)(void* context, const char* key, char* buffer, size_t buffer_size);

    /** @brief The broker's version. Valid for the life of the process. */
    xmq_str (*broker_version)(void* context);

    /**
     * @brief Where the broker's own accounts live, as a database URI.
     *
     * For an authenticator that answers out of that database - the same accounts the Users screen
     * edits. It is the broker's address to give: the interface writes there, so an extension that
     * took the address from a setting of its own could be pointed somewhere else and then admit
     * nobody the interface had added, with neither side able to notice.
     *
     * Copied like setting(), with the same return value; -1 on a broker whose accounts are not in a
     * database. An extension authenticating against something else - LDAP, a file, another
     * service - has no use for this and should not ask.
     */
    int64_t (*user_database_uri)(void* context, char* buffer, size_t buffer_size);

    /** @brief Drops every authorization decision the broker has cached, so that the next publish
     *         or subscribe asks again. Call it when the rules change; it is cheap and it is the
     *         only way a rule change takes effect on clients already connected. */
    void (*invalidate_acl)(void* context);

    /**
     * @brief Ask for facts about events that are not fields of xmq_event.
     *
     * The struct is fixed; this is not. A fact the broker learns to publish - the address a client
     * came from, why it went - becomes a name here rather than a new field, so adding one costs no
     * ABI version and no rebuild of anybody's extension. A name this broker does not publish comes
     * back NULL, which is a branch to write once rather than a build to make per version.
     *
     * @param names  Comma-separated, e.g. "remote_address, disconnect_reason". Replaces any
     *               previous request.
     *
     * @remarks Call this from start() and nowhere else. Values have to be captured as the event is
     *          queued, on the broker's own message threads, and asking for one afterwards cannot
     *          reach back for what was not taken. Only what is asked for is captured, so a name
     *          nobody wants costs nothing at all.
     */
    void (*want_event_attributes)(void* context, const char* names);

    /**
     * @brief One of those facts, for the event being delivered.
     *
     * @return The value, or a NULL xmq_str when it was not asked for, is not known for this event,
     *         or this broker does not publish that name. Valid until on_event() returns.
     */
    xmq_str (*event_attribute)(void* context, const xmq_event* event, const char* name);
} xmq_host;

/**
 * @brief Receives broker events. The extension implements this if it wants to watch.
 *
 * Never called on a thread that carries messages: the broker queues events and delivers them from
 * a thread of its own, so an observer that blocks costs the broker nothing but a growing queue,
 * and a full queue is dropped and counted rather than allowed to stall delivery. Events may
 * therefore arrive later than they happened, and under a flood some may not arrive at all.
 */
typedef struct xmq_observer
{
    void (*on_event)(void* instance, const xmq_event* event);
} xmq_observer;

/** @brief What kind of value a setting holds, so the interface can offer the right control and
 *         refuse a wrong value before it reaches the extension. */
typedef enum xmq_setting_type
{
    XMQ_SETTING_STRING = 0,
    XMQ_SETTING_INTEGER = 1,
    XMQ_SETTING_BOOLEAN = 2,
    XMQ_SETTING_CHOICE = 3, /**< One of the comma-separated names in `choices`. */

    /**
     * @brief A value that must never be displayed, logged, or written into a diagnostic.
     *
     * A connection URI with a password inside it is the ordinary case. Declaring it here is what
     * lets the interface mask it and the broker keep it out of its log - neither can tell a secret
     * from any other string on its own, and finding out the hard way is how secrets end up in
     * screenshots and bug reports.
     */
    XMQ_SETTING_SECRET = 4
} xmq_setting_type;

/**
 * @brief One setting an extension takes, as the extension describes it.
 *
 * Declared rather than discovered, for the same reason capabilities are: the broker sees only an
 * opaque map of strings, so without this the interface can offer nothing but a bare key/value table
 * where a misspelled key is found - if at all - when the extension next starts.
 *
 * Optional. An extension that declares nothing still works and still gets that bare table.
 */
typedef struct xmq_setting
{
    const char*      name;          /**< The key in xmq_extensions.conf. */
    const char*      label;         /**< For a person; NULL means use the name. */
    const char*      description;   /**< One line, shown beside the field. */
    uint32_t         type;          /**< An xmq_setting_type, fixed width like xmq_event::type. */
    const char*      default_value; /**< What the extension uses when it is not set; may be NULL. */
    const char*      choices;       /**< Comma-separated, for XMQ_SETTING_CHOICE; NULL otherwise. */
    uint8_t          required;      /**< Non-zero when the extension cannot start without it. */
    uint8_t          reserved[7];   /**< Zero-filled; keeps the struct's alignment explicit. */
} xmq_setting;

/** @brief What an extension implements. Declared, not inferred, so a capability cannot be
 *         forgotten silently and the broker need not call into extensions that have no opinion. */
typedef enum xmq_capability
{
    XMQ_CAP_OBSERVER = 1u << 0,      /**< Watches broker events. */
    XMQ_CAP_AUTHENTICATOR = 1u << 1, /**< Decides whether a client may connect. */
    XMQ_CAP_AUTHORIZER = 1u << 2     /**< Decides what a connected client may publish and subscribe to. */
} xmq_capability;

/**
 * @brief The answer any extension gives when its own subsystem failed.
 *
 * Its store, directory or remote service could not be reached, so it has no answer to give -
 * which is not the same as having no opinion, and certainly not the same as saying no. Both of
 * those would be wrong: abstaining hands the question to whoever is next and lets an outage look
 * like a client the extension had never heard of, while denying turns an outage into a wrong
 * answer the client will believe.
 *
 * One number across every decision enum, so that the rule reads the same wherever it appears.
 * Each enum still names it for itself, because C keeps those types distinct and an authorizer
 * must not be able to return an authenticator's answer by accident.
 *
 * What the broker then does is not shared, and is described with each capability below.
 */
#define XMQ_SUBSYSTEM_ERROR 3

/** @brief An authenticator's answer. */
typedef enum xmq_auth_decision
{
    /** No opinion. The broker asks the next authenticator, and finally its own accounts. An
     *  extension that only knows about one directory should say this for everyone else, rather
     *  than denying clients it was never meant to judge. */
    XMQ_AUTH_NOT_HANDLED = 0,
    XMQ_AUTH_ALLOW = 1,
    XMQ_AUTH_DENY = 2,
    /** The extension could not reach whatever it authenticates against. The broker refuses the
     *  connection - with ServerUnavailable under MQTT 5, ErrorServerNotAvailable under 3.x - and
     *  logs it. It does not ask the next authenticator and does not fall back to its own
     *  accounts: a store that cannot be read must not quietly become a store that says yes. */
    XMQ_AUTH_SUBSYSTEM_ERROR = XMQ_SUBSYSTEM_ERROR
} xmq_auth_decision;

/** @brief Everything an authenticator is told about a connecting client. */
typedef struct xmq_auth_request
{
    xmq_str client_id;
    xmq_str username;          /**< Empty when the client sent none. */
    xmq_str password;          /**< Empty when the client sent none. Not logged by the broker. */
    xmq_str remote_address;    /**< Peer address, for rules that depend on where a client is. */
    uint8_t protocol_version;  /**< 3 for MQTT 3.1, 4 for 3.1.1, 5 for MQTT 5. */
    uint8_t encrypted;         /**< 1 when the connection is TLS. */
    uint8_t reserved[6];
} xmq_auth_request;

/**
 * @brief Decides whether a client may connect.
 *
 * **This call may block.** It is made on a thread the broker keeps for authentication and never on
 * one that carries messages, precisely so that an LDAP or OIDC round trip - which is what real
 * authentication is - costs the connecting client its own latency and nothing else. Write it the
 * straightforward blocking way.
 *
 * What it costs is a thread from a small pool for the duration. An authenticator that never
 * returns holds one forever; when the pool is exhausted the broker refuses further connections
 * and says which extension it is waiting for.
 *
 * The broker asks each authenticator in configuration order and stops at the first that answers
 * ALLOW or DENY. If all of them answer NOT_HANDLED, the broker's own accounts decide.
 */
typedef struct xmq_authenticator
{
    xmq_auth_decision (*authenticate)(void* instance, const xmq_auth_request* request);
} xmq_authenticator;

/** @brief What a client is trying to do with a topic. */
typedef enum xmq_acl_action
{
    XMQ_ACL_PUBLISH = 0,
    XMQ_ACL_SUBSCRIBE = 1
} xmq_acl_action;

/** @brief An authorizer's answer, with the same abstention rule as authentication: say
 *         NOT_HANDLED about topics the extension has no rules for, rather than denying them. */
typedef enum xmq_acl_decision
{
    XMQ_ACL_NOT_HANDLED = 0,
    XMQ_ACL_ALLOW = 1,
    XMQ_ACL_DENY = 2,
    /** The extension could not reach whatever holds its rules. The one publish or subscribe being
     *  judged is refused and logged, and the connection stays up - unlike authentication, where
     *  the same fault ends the session, because here there is a session to keep. */
    XMQ_ACL_SUBSYSTEM_ERROR = XMQ_SUBSYSTEM_ERROR
} xmq_acl_decision;

/**
 * @brief Decides what a connected client may do, by group rather than by client.
 *
 * The two calls are split because they cost differently, and the split is the whole reason this is
 * affordable.
 *
 * resolve_group() runs once per connection, on the authentication thread, and **may block** - it
 * is where a directory is asked which groups a client belongs to. Its answer is a group name the
 * extension chooses; the broker keeps it on the session and never interprets it.
 *
 * authorize() is asked about a **group and a topic**, never about a client, and the broker caches
 * the answer under exactly that pair. A thousand clients in one group publishing to one topic ask
 * once between them; every publish after that is a hash lookup on the broker's side and no call at
 * all. This is why the rules must not depend on the individual client - if they did, the cache key
 * would be the client and the cache would be worth nothing.
 *
 * Because it is called with a lock held on the cache and on a thread carrying messages,
 * authorize() **must not block** and must not call back into the broker. Read a prepared table and
 * return. Everything expensive belongs in start() or in resolve_group().
 *
 * Rules that change at runtime are the extension's business: call xmq_host::invalidate_acl to
 * drop what the broker has cached, and the next use asks again.
 */
typedef struct xmq_authorizer
{
    /**
     * @brief Names the group a connecting client belongs to. May block.
     *
     * Writes the name into group, NUL-terminated, and returns its length without the NUL. Returning
     * 0 puts the client in no group, for which authorize() is still asked.
     *
     * A name that does not fit is not cut short: the extension returns its full length, which is
     * group_size or more, and the broker refuses the connection. Cut to fit, two groups could come
     * out with the same name - and so with each other's rights.
     */
    size_t (*resolve_group)(void* instance, const xmq_auth_request* request, char* group, size_t group_size);

    /** @brief May this group do this to this topic? Must not block. */
    xmq_acl_decision (*authorize)(void* instance, const xmq_str* group, const xmq_str* topic,
                                  xmq_acl_action action);
} xmq_authorizer;

/**
 * @brief One extension, as the broker sees it.
 *
 * create() returns the extension's own state, which the broker passes back to everything else and
 * finally to destroy(). Capability pointers are NULL when not implemented.
 */
typedef struct xmq_extension
{
    size_t      struct_size;       /**< sizeof(xmq_extension) as the extension compiled it: the
                                        broker reads no field beyond it. */
    const char* name;              /**< Short, stable; appears in the log and in configuration. */
    const char* version;           /**< The extension's own version, for the log. */

    /**
     * @brief One line saying what this extension is for, shown wherever it is listed.
     *
     * NULL when the extension does not say. The broker keeps the last one it saw in the extension's
     * configuration entry, so an extension that is switched off can still describe itself without
     * its library being loaded to ask - which is what "switched off" is supposed to mean.
     */
    const char* description;

    void* (*create)(const xmq_host* host);
    void (*destroy)(void* instance);

    /** @brief Begin work. Threads an extension needs are its own to start here. */
    xmq_status (*start)(void* instance);

    /** @brief Stop and join everything started. The broker will not call anything after this. */
    xmq_status (*stop)(void* instance);

    /**
     * @brief This extension's settings have changed; read them again.
     *
     * Called on a broker that is running and serving, so that a long-lived installation need not be
     * restarted to change a setting - and so concurrently with the extension's other calls: see
     * Threads, at the top of this file.
     *
     * Only the settings change. The library is the one that was loaded, the instance is the one
     * that was created, and whatever the extension holds open stays open - that is the difference
     * between this and being stopped and started, and the reason this exists.
     *
     * Returning XMQ_ERROR leaves the extension running with what it had. An extension that cannot
     * apply a change must say so rather than half-apply it: half a configuration is the state
     * nobody can reason about.
     *
     * NULL when the extension does not implement it, and then a settings change reaches it only
     * through a restart of the broker.
     */
    xmq_status (*reload)(void* instance);

    /**
     * @brief The accounts in the broker's user database have changed; forget what was cached.
     *
     * Called after the interface adds, changes or removes an account or a group membership. Not
     * reload(): that one re-reads settings, and in an extension that authenticates against a
     * database it opens the store, probes it and may move accounts between two of them. This is
     * the cheap half - drop the cached decisions and answer from the store again - and it happens
     * whenever somebody presses Save.
     *
     * Without it a password stays usable for as long as the extension caches it, which is exactly
     * the wrong thing to be told about late: an account is usually revoked because somebody should
     * not be connecting now.
     *
     * Called from the thread that made the change, so an extension that blocks here delays the
     * interface. NULL when the extension caches nothing or authenticates against something the
     * broker does not edit.
     */
    void (*accounts_changed)(void* instance);

    uint32_t                 capabilities;  /**< Bitwise OR of xmq_capability. */
    const xmq_observer*      observer;      /**< NULL when the extension does not watch events. */
    const xmq_authenticator* authenticator; /**< NULL when the extension does not authenticate. */
    const xmq_authorizer*    authorizer;    /**< NULL when the extension does not authorize. */

    /**
     * @brief The settings this extension takes, or NULL when it declares none.
     *
     * What makes a configuration screen possible: labels, types, defaults, and which values are
     * refused before they reach the extension rather than after.
     */
    const xmq_setting* settings;
    size_t             setting_count;
} xmq_extension;

/**
 * @brief The one symbol an extension exports.
 *
 * Called once, before anything else, on the thread that loads the library. It must not start
 * threads, open sockets or read files - the broker may be asking only in order to refuse the
 * version. Return NULL to decline being loaded.
 *
 * @param abi_major   The broker's ABI major version.
 * @param abi_minor   The broker's ABI minor version.
 */
typedef const xmq_extension* (*xmq_extension_describe_fn)(uint32_t abi_major, uint32_t abi_minor);

#define XMQ_EXTENSION_DESCRIBE_SYMBOL "xmq_extension_describe"

#ifdef __cplusplus
} // extern "C"
#endif

#endif // XMQ_EXTENSION_H
