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

#include "../Settings/LogSubject.h"
#include "extension/xmq_extension.h"

#include <sptk5/Logger.h>
#include <sptk5/threads/SynchronizedQueue.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

namespace xmq {

/**
 * @brief Loads extensions and delivers broker events to them.
 *
 * One instance per broker. Extensions are named in the configuration; nothing is discovered by
 * scanning a directory, because a broker that loads whatever is in a folder is a broker whose
 * behaviour depends on what someone left there.
 *
 * Events reach an extension through a queue drained by this class's own thread. That is the whole
 * safety argument for the observer capability: the reactor, the receive workers and the send
 * workers only ever push, the queue is bounded, and an extension that is slow costs dropped events
 * rather than delivery latency. The broker's own path is tens of microseconds - a single blocking
 * call into somebody else's code on that path would be the larger number.
 */
class ExtensionHost;

/// What the authorizers, taken together, decided about a group and a topic.
enum class AclDecision : uint8_t
{
    NotHandled, ///< No extension had an opinion; the broker's own rules stand.
    Allow,
    Deny,
    /// An authorizer could not reach whatever holds its rules. The one operation is refused; the
    /// session is not. Never cached - an outage lasting a second would otherwise be remembered
    /// for the life of the group.
    SubsystemError
};

/**
 * @brief The permissions of one group, and everything already learned about them.
 *
 * A session is given one of these when it connects and holds it for its lifetime, so the hot path
 * never looks up a group by name and never hashes a client id. What it does is ask this object
 * about a topic, which is a hash lookup on a map shared by every session in the group: the first
 * client to publish to a topic pays for the question, and every client after it - and every later
 * message from the same one - reads the answer.
 *
 * That sharing is the entire performance argument for group-based rules. Per-client rules would
 * need a map per client, which is the same lookup with none of the sharing and a great deal more
 * memory.
 */
class AclGroup
{
public:
    AclGroup(ExtensionHost* host, std::string name)
        : m_host(host)
        , m_name(std::move(name))
    {
    }

    /// May this group do this to this topic? Cheap after the first time it is asked.
    [[nodiscard]] AclDecision authorize(std::string_view topic, xmq_acl_action action);

    /// The name the extension gave this group. Empty when no extension recognised the client.
    [[nodiscard]] const std::string& name() const
    {
        return m_name;
    }

private:
    friend class ExtensionHost;

    void forget();

    /// Lets a topic be looked up as a string_view, so the hot path does not build a std::string
    /// on its way to a cache hit.
    struct TopicHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view topic) const noexcept
        {
            return std::hash<std::string_view> {}(topic);
        }
    };

    using Decisions = std::unordered_map<std::string, AclDecision, TopicHash, std::equal_to<>>;

    ExtensionHost*            m_host;
    std::string               m_name;

    /// Identity for the per-thread decision cache, and the reason that cache cannot key on the
    /// address of the group.
    ///
    /// Groups are made when a client connects and destroyed when it goes, so an allocator hands
    /// the same address out again in the ordinary course of a broker's day. A cache entry left
    /// behind by the previous occupant matches the new one on every field - same pointer, same
    /// generation, since every group starts at 1 - and the new group is served the old one's
    /// answer. That is one group's permissions given to another, in either direction.
    ///
    /// Never reused, so a stale entry can only miss. See [[the socket pool]], which had the same
    /// fault with Socket* as a demux key and the same cure.
    const uint64_t m_id {nextId()};

    static uint64_t nextId();

    /// Bumped by forget(). Read on the hot path to tell a still-good thread-local answer from one
    /// that predates a rule change; read-only in the steady state, so the line it lives on stays
    /// shared in every core's cache.
    std::atomic<uint64_t> m_generation {1};

    mutable std::shared_mutex m_lock;
    Decisions                 m_publish;
    Decisions                 m_subscribe;
};

class ExtensionHost
{
public:
    /// One extension as configured: where to load it from and what to tell it about itself.
    struct Configured
    {
        std::string                        m_name;     ///< Reported name; must match what the library says.
        std::filesystem::path              m_library;  ///< Path to the shared library.
        std::map<std::string, std::string> m_settings; ///< The extension's own configuration block.

        /**
         * @brief Whether the broker may serve MQTT without this extension.
         *
         * Operator policy rather than the author's, so it is here and not in the ABI: the same
         * library is optional on one installation and load-bearing on another. Default false keeps
         * the rule the broker has always had - a broken extension does not stop it - and an
         * extension that is the only way clients are authenticated says so here.
         */
        bool m_required {false};

        /// Which file this entry came from, so the screen can say where to edit it - the single
        /// file or one fragment among several is not something an operator should have to hunt for.
        std::filesystem::path m_source;

        /// Whether the configuration asks for it to run. A disabled entry is still read and still
        /// listed - the screen promises every extension, and one that is switched off is exactly
        /// the one somebody came to look at - but its library is never opened, which is what being
        /// switched off has to mean.
        bool m_enabled {true};
    };

    explicit ExtensionHost(sptk::LogEngine& logEngine, std::string brokerVersion);

    /**
     * @brief Read the extension list from a file beside the broker's configuration.
     *
     * Its own file rather than a section of xmq_server.conf, for the same reason accounts have
     * one: the broker's configuration is generated from xmq.wsdl and rewritten from that model, so
     * a section the model does not know about would be dropped the first time anything was saved.
     * It is also deployment-level configuration - it names paths on this machine - and does not
     * belong in an interface that edits broker behaviour.
     *
     * A missing file means no extensions, which is not an error. A malformed one is reported and
     * treated the same way: the broker starts.
     *
     * @param configurationPath  Path of the broker's configuration file.
     * @param logEngine          Where to report a file that cannot be read.
     */
    [[nodiscard]] static std::vector<Configured> readConfiguration(
        const std::filesystem::path& configurationPath, sptk::LogEngine& logEngine);

    /// Where readConfiguration() looks: xmq_extensions.conf beside the broker's configuration.
    /**
     * @brief The directory of configuration fragments, beside the broker's configuration.
     *
     * Every *.conf in it is read after xmq_extensions.conf, in name order. It exists so that an
     * extension can install its own entry instead of asking somebody to merge one into a shared
     * file by hand - and so that installing a second extension does not touch the first one's.
     *
     * Name order, not directory order: the sequence extensions are asked in is part of what they
     * mean, and iteration order is whatever the filesystem gives. A numeric prefix in the name is
     * how that order is chosen.
     */
    [[nodiscard]] static std::filesystem::path configurationDirectoryFor(
        const std::filesystem::path& configurationPath);

    [[nodiscard]] static std::filesystem::path configurationPathFor(
        const std::filesystem::path& configurationPath);

    /**
     * @brief Create the live fragments an installation shipped templates for.
     *
     * An extension's entry in xmq_extensions.d is a live file - it says whether the extension is
     * on, and holds settings the operator has answered. A package cannot own such a file: every
     * upgrade would put its own copy back and quietly undo those answers, which is what installing
     * 50-user-database.conf did until this existed. So an installation ships a .conf.template
     * instead, and the first broker to find no live file beside it copies one - the same bargain
     * the broker's own configuration is under, and for the same reason.
     *
     * Every <name>.conf.template with no <name>.conf beside it, so an extension outside this
     * repository gets the same treatment by shipping a template. Never afterwards: a live file that
     * exists is the operator's, whatever it says.
     *
     * @param configurationPath  Path of the broker's configuration file.
     * @param logEngine          Where to report what was created, and what could not be.
     */
    static void createConfigurationFragments(const std::filesystem::path& configurationPath,
                                             sptk::LogEngine&             logEngine);

    ~ExtensionHost();

    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;

    /**
     * @brief Load and start everything configured.
     *
     * A library that will not load, or refuses the ABI version, or names itself something other
     * than the configuration expects, is reported and skipped - the broker starts without it. An
     * extension is an addition to a broker, never a condition for one.
     */
    void start(const std::vector<Configured>& extensions);

    /**
     * @brief Apply a changed configuration to a broker that is running.
     *
     * What a broker running for months needs: a setting changed without dropping every client to
     * pick it up. Only the settings of extensions that are already loaded, and only those whose
     * block actually differs - an extension that has not changed is not disturbed at all.
     *
     * An extension is told through its reload() callback, which is optional: one that does not
     * implement it keeps what it had, and its settings change when the broker next starts. Saying
     * so is the honest answer, and better than a callback that pretends.
     *
     * Adding and removing extensions is not this, and is deliberately not here yet: it needs the
     * loaded list to be swapped under readers on the message path, which is a different piece of
     * work from replacing a map.
     *
     * @return What happened, one line per extension, for the log and for whoever pressed the
     *         button.
     */
    /**
     * @brief What an action did, with what failed kept apart from what happened.
     *
     * One list of lines cannot say whether it went well: the interface showed a refused database
     * URI in an information window, with the same icon a success gets, because everything the
     * broker answered looked alike. Deciding by looking for the word "refused" in English prose
     * would be worse than the bug. So the classification is made where it is known - here - and
     * carried rather than reconstructed.
     */
    struct Report
    {
        sptk::Strings m_problems; ///< What did not happen, each line saying why.
        sptk::Strings m_notes;    ///< What did.

        [[nodiscard]] bool failed() const
        {
            return !m_problems.empty();
        }

        /// Merges another report into this one, keeping both halves apart.
        Report& operator+=(const Report& other)
        {
            for (const auto& line: other.m_problems)
            {
                m_problems.push_back(line);
            }
            for (const auto& line: other.m_notes)
            {
                m_notes.push_back(line);
            }
            return *this;
        }

        /// A problem, as a report of its own.
        static Report problem(const sptk::String& line)
        {
            return {.m_problems = {line}, .m_notes = {}};
        }

        /// Something that went as asked.
        static Report note(const sptk::String& line)
        {
            return {.m_problems = {}, .m_notes = {line}};
        }
    };

    Report reloadSettings(const std::vector<Configured>& extensions);

    /**
     * @brief Tell the extensions where the broker's accounts live.
     *
     * Answered back to any extension that asks through the ABI's user_database_uri(). Set at start
     * and whenever the configuration changes it, so that the address an authenticator uses is the
     * one the interface writes to - the two used to be configured separately and could differ
     * without either side noticing.
     */
    void userDatabaseUri(std::string uri);

    /**
     * @brief An account or a group membership has changed; extensions that cached it must forget.
     *
     * Called from the interface's own path after a successful write, not from anything that
     * carries messages. Cheap by construction: it asks each extension to drop what it cached, and
     * nothing else - see accounts_changed() in the ABI for why this is not reload().
     */
    void accountsChanged();

    /**
     * @brief Switch an extension on, on a running broker.
     *
     * Loads its library if this is the first time; otherwise creates a fresh instance beside the
     * library that is still mapped from when it was switched off. Then start(), and only then does
     * it appear in the snapshot: an extension is never asked anything before it has started.
     *
     * @return What happened, for the log and for whoever pressed the button.
     */
    /// One declared setting, as the screen needs it: what the extension said about it, and what
    /// the configuration currently holds.
    struct SettingDescription
    {
        std::string      m_name;
        std::string      m_label;
        std::string      m_description;
        xmq_setting_type m_type {XMQ_SETTING_STRING};
        std::string      m_defaultValue;
        std::string      m_choices;
        bool             m_required {false};
        std::string      m_value;    ///< What is configured now; empty when it is not set.
        bool             m_declared {true};
    };

    /// Everything the interface shows about one extension.
    struct Description
    {
        std::string                     m_name;
        std::string                     m_version;
        std::string                     m_description;
        std::filesystem::path           m_library;
        std::filesystem::path           m_source;
        bool                            m_running {false};
        bool                            m_required {false};
        uint32_t                        m_capabilities {0};
        std::vector<SettingDescription> m_settings;
        uint64_t                        m_eventsDelivered {0};
        uint64_t                        m_admitted {0};
        uint64_t                        m_refused {0};
        uint64_t                        m_storeErrors {0};
    };

    /**
     * @brief Every extension the broker has loaded, running or not.
     *
     * Settings are reported declared-first, in the order the extension declared them, each carrying
     * what the configuration holds for it. A key that is set but was never declared is reported too,
     * marked undeclared - hiding it would hide a typo, which is the commonest reason a setting has
     * no effect.
     */
    [[nodiscard]] std::vector<Description> describe() const;

    /**
     * @brief Write an extension's settings back to the file its entry came from.
     *
     * Only that entry's `settings` block is touched: everything else in the file - the other
     * entries, `enabled`, `required`, and the `_comment` lines somebody wrote for the next reader -
     * is carried through unchanged.
     *
     * A setting the extension declared as a secret whose incoming value is the mask the interface
     * showed is left as it was. The screen never receives a secret, so it can only send back what
     * it was given, and writing that would replace a password with a row of asterisks - which is
     * how a configuration quietly stops working.
     *
     * Writing only. Applying is the caller's next step, so that a value that cannot be written is
     * never applied either.
     */
    /**
     * @brief Write whether the configuration asks for this extension to run.
     *
     * What makes the Disable button mean what it says. Without it the button only stops the
     * extension: the entry still reads enabled, so the next reload - or the next restart - starts
     * it again, and the operator is left wondering what undid their decision.
     */
    Report writeEnabled(const std::string& name, bool enabled);

    Report writeSettings(const std::string&                        name,
                         const std::map<std::string, std::string>& settings);

    Report enable(const Configured& configured);

    /**
     * @brief Switch an extension off, on a running broker.
     *
     * Taken out of the snapshot first, so nothing new reaches it; then the calls already inside it
     * are waited out; then stop() and its instance destroyed. The library stays mapped - see
     * m_loaded for why nothing is ever dlclose()d.
     *
     * Refuses to stop an extension the configuration marks required: without it the broker does not
     * serve MQTT at all, so switching it off here would be a way to stop serving that does not say
     * so. Change the configuration and restart if that is what is meant.
     */
    Report disable(const std::string& name);

    /// Stop and unload everything, in reverse order of loading.
    void stop();

    /// True when at least one loaded extension watches events, so callers can skip building one.
    [[nodiscard]] bool watching() const
    {
        return m_watching.load(std::memory_order_relaxed);
    }

    /**
     * @brief A fact about an event that is not a field of xmq_event.
     *
     * Names rather than fields, so that the broker learning to publish one more thing is not a
     * change to the ABI and not a rebuild of anybody's extension - see want_event_attributes.
     * A caller offers what it happens to know; only what some extension asked for is copied.
     */
    struct EventAttribute
    {
        std::string_view m_name;
        std::string_view m_value;
    };

    using EventAttributes = std::initializer_list<EventAttribute>;

    /// The names this broker publishes. Unknown names asked for by an extension are reported once
    /// and then ignored, which is how a typo is told from a fact this broker does not have.
    static constexpr std::string_view remoteAddressAttribute = "remote_address";

    /// What an error event carries. Always captured, never asked for: they are not extra facts
    /// about the event, they are what it says - and an error nobody can identify is worse than no
    /// error at all. Free by construction, because errors are rare and the frequent ones live
    /// under a subject that is switched off by default.
    /**
     * @brief What the interface is shown instead of a secret, and what it sends back untouched.
     *
     * One constant, because the two ends have to agree exactly: the screen never receives a secret,
     * so an unchanged one comes back as this, and writing it would replace a password with a row of
     * asterisks.
     */
    static constexpr std::string_view secretMask = "********";

    static constexpr std::string_view errorSubjectAttribute = "error_subject";
    static constexpr std::string_view errorReasonAttribute = "error_reason";
    static constexpr std::string_view errorMessageAttribute = "error_message";

    /**
     * @brief Something the broker refused or could not do.
     *
     * Raised where the broker already logs the failure, so that the two cannot drift apart, and
     * governed by the same switch as the subject's ordinary events: an installation that does not
     * want publish events does not want publish failures either, and the frequent failure - a
     * rejected PUBLISH - is exactly the one that must not be paid for by default.
     *
     * @param subject   Where it happened; also the switch that governs it.
     * @param reason    A stable token for code to match on, never a sentence.
     * @param message   The sentence, for a person.
     * @param clientId  The client it concerns, when there is one.
     * @param username  The name it was attempted with, when there is one - empty for anonymous.
     */
    void publishError(LogSubject subject, std::string_view reason, std::string_view message,
                      std::string_view clientId = {}, std::string_view username = {},
                      EventAttributes attributes = {});

    /**
     * @brief Hand an event to the observers.
     *
     * Called from whichever thread the event happened on, including hot ones. Copies what it needs
     * and returns; nothing in an extension runs here. Silently drops when the queue is full, and
     * counts the drops.
     *
     * @param attributes  What the caller knows beyond the fields. Copying happens only for the ones
     *                    an extension asked for, so offering one nobody wants costs a comparison.
     */
    void publishEvent(xmq_event_type type, std::string_view clientId, std::string_view username,
                      std::string_view topic, size_t payloadSize, uint8_t qos, bool retain,
                      EventAttributes attributes = {});

    /**
     * @brief Which subjects produce events, as a bit per LogSubject.
     *
     * By subject rather than by event type, because an error is an event of whichever subject it
     * happened in - a refused CONNECT is a connect event, a rejected PUBLISH a publish one - and
     * one switch should govern both what a subject does and what it fails to do.
     *
     * Set from the configuration before start(). The default filters nothing: which subjects an
     * installation wants is policy and belongs to the broker, while this is the mechanism - and a
     * mechanism that silently dropped events its caller asked for would be the harder fault to
     * find of the two.
     *
     * An event nobody wants costs the thread that raised it three string copies into a bounded
     * queue. Once per session that is nothing; per message it is the broker's hottest path.
     */
    void enableEvents(uint32_t subjects)
    {
        m_enabledEvents.store(subjects, std::memory_order_relaxed);
    }

    /// The bit for one subject, for building the mask above.
    [[nodiscard]] static constexpr uint32_t subjectBit(const LogSubject subject)
    {
        return 1U << static_cast<unsigned>(subject);
    }

    /// The subject an event of this type belongs to, and so the switch that governs it.
    [[nodiscard]] static constexpr LogSubject subjectOf(const xmq_event_type type)
    {
        switch (type)
        {
            case XMQ_EVENT_CLIENT_CONNECTED:
                return LogSubject::Connect;
            case XMQ_EVENT_CLIENT_DISCONNECTED:
                return LogSubject::Disconnect;
            case XMQ_EVENT_SUBSCRIBED:
                return LogSubject::Subscribe;
            case XMQ_EVENT_UNSUBSCRIBED:
                return LogSubject::Unsubscribe;
            case XMQ_EVENT_PUBLISHED:
                return LogSubject::Publish;
            case XMQ_EVENT_ERROR:
                break; // carries its own subject; publishError() is how it is raised
        }
        return LogSubject::ServerEvents;
    }

    /// Events dropped because the queue was full. Reported at shutdown and through $SYS.
    [[nodiscard]] uint64_t droppedEvents() const
    {
        return m_dropped.load(std::memory_order_relaxed);
    }

    /// True when some loaded extension decides who may connect, so the CONNECT path can take the
    /// direct route when none does - which is every broker that has no such extension configured.
    [[nodiscard]] bool authenticating() const
    {
        return m_authenticating.load(std::memory_order_relaxed);
    }

    /// What the authenticators, taken together, decided.
    enum class AuthDecision : uint8_t
    {
        NotHandled, ///< No extension had an opinion; the broker's own accounts decide.
        Allow,
        Deny,
        Unavailable,   ///< No thread was free to ask. Treated as a denial, and said so in the log.
        SubsystemError ///< An authenticator could not reach its store. Refused, and not fallen back on.
    };

    /// Everything an authenticator is told, owned by the broker for the life of the request.
    struct AuthRequest
    {
        std::string m_clientId;
        std::string m_username;
        std::string m_password;
        std::string m_remoteAddress;
        uint8_t     m_protocolVersion {};
        bool        m_encrypted {};
    };

    /**
     * @brief Ask the extensions whether a client may connect, without blocking the caller.
     *
     * Returns at once. The request is handed to a thread the host keeps for authentication, the
     * authenticators are asked there in configuration order, and @p answer is called with the
     * first ALLOW or DENY - or NotHandled when none of them had an opinion.
     *
     * This exists because real authentication is a network call. Asking on the receive thread
     * would stall every other client on that thread for the duration of somebody else's directory
     * server, and the broker's own path is measured in tens of microseconds.
     *
     * The group a client belongs to is resolved on the same thread and handed back with the
     * decision, because it is the same question asked of the same directory at the same moment.
     * Giving authorization a hop of its own would double what a connection waits for and buy
     * nothing.
     *
     * @param request   Copied; the caller need not keep it alive.
     * @param answer    Called on an authentication thread, exactly once. The group is null when
     *                  nothing authorizes, or when the client was refused.
     */
    void authenticate(AuthRequest request,
                      std::function<void(AuthDecision, std::shared_ptr<AclGroup>)> answer);

    /// True when some loaded extension decides what a client may publish and subscribe to. False
    /// for every broker with no such extension, and then the permission checks below are never
    /// reached at all.
    [[nodiscard]] bool authorizing() const
    {
        return m_authorizing.load(std::memory_order_relaxed);
    }

    /**
     * @brief Which group does this client belong to?
     *
     * Called once per connection, on an authentication thread - the same hop that already asks the
     * authenticators, so authorization costs the CONNECT path no additional wait. Extensions are
     * asked in configuration order and the first that names a group wins.
     *
     * Never returns null: a client no extension recognised gets the unnamed group, which has its
     * own cache and its own answers, so an extension that denies by default still denies it once
     * rather than once per message.
     *
     * @param request   Client authentication request.
     * @param refused   Set when an extension names a group too long to hold; the client is refused.
     * @return The client's group, including the unnamed group when none is supplied.
     */
    [[nodiscard]] std::shared_ptr<AclGroup> resolveGroup(const AuthRequest& request, bool& refused);

    /// Drops every cached decision, so that changed rules reach clients already connected. Called
    /// by an extension through the host table, and never on a hot path.
    void invalidateAcl();

private:
    friend class AclGroup;

    /// Asks the authorizers about one group and topic. Called only on a cache miss.
    [[nodiscard]] AclDecision askAuthorizers(const std::string& group, std::string_view topic,
                                             xmq_acl_action action);

    /// An event, owning its strings: the originals belong to the thread that raised it.
    struct QueuedEvent
    {
        xmq_event_type m_type {};
        uint64_t       m_timestampUs {};
        std::string    m_clientId;
        std::string    m_username;
        std::string    m_topic;
        size_t         m_payloadSize {};
        uint8_t        m_qos {};
        bool           m_retain {};

        /// Only the attributes some extension asked for, copied at push time: the originals belong
        /// to the thread that raised the event and are gone by the time it is delivered.
        std::vector<std::pair<std::string, std::string>> m_attributes;
    };

    /// A loaded library and the instance made from it.
    struct Loaded
    {
        std::string                        m_name;
        std::filesystem::path              m_library;
        void*                              m_handle {nullptr}; ///< dlopen handle.
        const xmq_extension*               m_table {nullptr};
        void*                              m_instance {nullptr};
        bool                               m_started {false};

        /// From the configuration: the broker does not serve MQTT without this one, so it may not
        /// be switched off from the interface.
        bool                  m_required {false};
        std::filesystem::path m_source;

        /// The last thing this extension logged as an error, kept so that a call which only
        /// answers yes or no can still say why it said no.
        ///
        /// The ABI's reload() and start() return a code and nothing else, so "it refused" was all
        /// the interface could be told - and the reason, which the extension had already written
        /// to the log, stayed in the log. Recorded here and read back only across the one call
        /// that just failed, so what is shown was caused by that call and not by something the
        /// extension said ten minutes ago.
        std::mutex  m_lastErrorLock;
        std::string m_lastError;

        /// What this extension has actually done. Relaxed throughout: they are read for a screen,
        /// where being one behind is not worth an ordering constraint on the paths that write them.
        std::atomic<uint64_t> m_eventsDelivered {0};
        std::atomic<uint64_t> m_admitted {0};
        std::atomic<uint64_t> m_refused {0};
        std::atomic<uint64_t> m_storeErrors {0};

        /**
         * @brief Calls currently inside this extension.
         *
         * What makes switching one off on a running broker safe. Marking it inactive stops new
         * calls; this is how the ones already in there are waited out before stop() runs, because
         * stop() releases what a call in flight is still using.
         *
         * Touched only when the broker really calls into the extension - never on a cached
         * authorization, which is the one path per message.
         */
        std::atomic<int> m_inFlight {0};
        /// Guards the two below against a live reload replacing them while the extension reads
        /// them. Taken only by setting(), which an extension calls in start() and reload() rather
        /// than on any path that carries messages.
        mutable std::mutex                 m_settingsLock;
        std::map<std::string, std::string> m_settings;
        ExtensionHost*                     m_host {nullptr};
        xmq_host                           m_hostTable {};
    };

    /// One authentication in flight, with the callback that is owed an answer.
    struct PendingAuth
    {
        AuthRequest                                                  m_request;
        std::function<void(AuthDecision, std::shared_ptr<AclGroup>)> m_answer;
        /// When it was queued, so a request nobody could reach in time is refused instead of being
        /// carried out for a client that has long since given up.
        std::chrono::steady_clock::time_point m_queuedAt {std::chrono::steady_clock::now()};
    };

    [[nodiscard]] std::unique_ptr<Loaded> load(const Configured& configured);
    void                                  unload(Loaded& loaded);
    void                                  deliveryThread();
    void                                  authenticationThread();
    [[nodiscard]] AuthDecision            askAuthenticators(const AuthRequest& request);

    // The three services an extension is given. Static, because they are C function pointers with
    // the Loaded record as their context.
    static void    hostLog(void* context, xmq_log_priority priority, const char* message);
    static int64_t hostSetting(void* context, const char* key, char* buffer, size_t bufferSize);

    /// The last error an extension logged, cleared as it is read - see Loaded::m_lastError.
    static std::string takeLastError(Loaded& loaded);
    static xmq_str hostBrokerVersion(void* context);
    static int64_t hostUserDatabaseUri(void* context, char* buffer, size_t bufferSize);
    static void    hostInvalidateAcl(void* context);

    /// Rebuilds the snapshot readers iterate from the started entries of m_loaded, and recomputes
    /// which capabilities are live. Call with m_loadedLock held.
    void publishActiveUnlocked();

    /// Starts the delivery thread and the authentication pool if the capabilities now need them and
    /// they are not running. Idempotent: switching a second observer on must not start a second
    /// delivery thread.
    void startWorkerThreads();

    /// Waits for the calls already inside @p loaded to leave, so that stop() cannot release what one
    /// of them is still using. Returns false if they did not leave in time, and then nothing is
    /// stopped: a leaked instance is recoverable, a use-after-free is not.
    [[nodiscard]] static bool quiesce(Loaded& loaded, const sptk::Logger& logger);
    /// Reads one file into @p extensions. A malformed one costs only itself.
    /**
     * @brief Reads one file into @p extensions. A malformed one costs only itself.
     * @param seenIn  Which file each name has already come from. The first entry of a name wins and
     *                a later one is refused: two entries of one name are two instances of one
     *                library, which reads as the broker misbehaving rather than as a configuration
     *                that says one thing twice.
     */
    static void readConfigurationFile(const std::filesystem::path& path, const sptk::Logger& logger,
                                      std::vector<Configured>&                     extensions,
                                      std::map<std::string, std::filesystem::path>& seenIn);

    static void    hostWantEventAttributes(void* context, const char* names);
    static xmq_str hostEventAttribute(void* context, const xmq_event* event, const char* name);

    /// The bit for a name this broker publishes, or zero for one it does not.
    [[nodiscard]] static uint32_t attributeBit(std::string_view name);

    /// How long a group name an extension may return. Generous for a name; small enough that the
    /// buffer lives on the authentication thread's stack.
    static constexpr size_t MaxGroupNameLength = 128;

    /// Deep enough to absorb a burst, shallow enough that a stuck extension cannot eat memory.
    static constexpr size_t MaxQueuedEvents = 16384;

    sptk::Logger                         m_logger;
    std::string                          m_brokerVersion;
    /**
     * @brief Everything ever loaded, in configuration order. Append-only.
     *
     * Nothing is ever erased and no library is ever dlclose()d: a switched-off extension keeps its
     * code mapped and loses only its instance. That is tens of kilobytes against the one failure
     * mode that cannot be recovered from - a thread inside a library that has just been unmapped.
     *
     * Mutated only under m_loadedLock. Readers never touch it; they iterate the snapshot below.
     */
    std::vector<std::unique_ptr<Loaded>> m_loaded;

    /// Entries the configuration switched off: known, listed, never loaded. Guarded by the same
    /// lock as m_loaded, because switching one on moves it from here to there.
    std::vector<Configured> m_configuredOnly;

    /// Guards m_loaded, m_configuredOnly and the swapping of m_active. Never taken on a path that
    /// carries messages.
    mutable std::mutex m_loadedLock;

    /// Where the broker keeps its accounts, as the configuration says. Guarded because it is
    /// written when the configuration changes and read by extensions on their own threads.
    mutable std::mutex m_userDatabaseUriLock;
    std::string        m_userDatabaseUri;

    using ActiveList = std::vector<Loaded*>;

    /**
     * @brief Every snapshot ever published, kept for as long as the host lives.
     *
     * A reader holds a bare pointer to the current one, so the list it is walking must not be freed
     * under it. Nothing here is ever released - the same choice as never unloading a library, and
     * for the same reason: a few hundred bytes per reconfiguration against a use-after-free on the
     * broker's own threads. Reconfiguring is an operator pressing a button, not something that
     * happens per message.
     *
     * std::atomic<std::shared_ptr<>> would say this more directly and was how it was first written.
     * libc++ does not implement it, so a FreeBSD build with Clang would not compile - and that is
     * where FreeBSD is going now that gcc15 is gone from it.
     */
    std::vector<std::unique_ptr<const ActiveList>> m_snapshots;

    /**
     * @brief The extensions that are currently on, as readers see them.
     *
     * A snapshot rather than the list itself, because the delivery thread, the authentication pool
     * and the authorization path all iterate this without a lock, and appending to a vector under
     * them would move what they are iterating. Swapping a whole snapshot costs the readers one
     * atomic load and cannot be observed half-done.
     *
     * Null until the first publish, and null again after stop(), so every reader checks.
     */
    std::atomic<const ActiveList*> m_active {nullptr};
    sptk::SynchronizedQueue<QueuedEvent> m_events;
    std::atomic_size_t                   m_queued {0};
    std::atomic<uint64_t>                m_dropped {0};

    /**
     * @brief Which attributes any extension has asked for, as a bit per known name.
     *
     * A mask rather than a set of strings because it is read on the publish path, which is the
     * broker's hottest: an extension that wants nothing costs one relaxed load there. Written only
     * from start(), which happens before any event, so the read needs no ordering.
     */
    std::atomic<uint32_t> m_wantedAttributes {0};

    /// Every type, until the broker says otherwise - see enableEvents().
    std::atomic<uint32_t> m_enabledEvents {~0U};

    /// The event being delivered, and what is known about it. Touched only on the delivery thread -
    /// which is also the thread an extension's event_attribute() call arrives on.
    const xmq_event*                                        m_deliveringEvent {nullptr};
    const std::vector<std::pair<std::string, std::string>>* m_deliveringAttributes {nullptr};
    std::atomic_bool                     m_watching {false};
    std::atomic_bool                     m_authenticating {false};
    std::atomic_bool                     m_authorizing {false};
    std::atomic_bool                     m_terminated {false};
    std::thread                          m_deliveryThread;

    /// Authentication runs here and nowhere else. Small on purpose: it bounds how much of a
    /// hanging directory server the broker will absorb before it starts refusing connections,
    /// which is a better failure than an unbounded number of threads waiting on one.
    /// The upper bound. The work is a database round trip and a costed KDF, so more threads than
    /// cores buy nothing on a store that answers, and on one that hangs they only add waiting.
    static constexpr size_t MaxAuthenticationThreads = 16;
    /// The lower bound, kept for a store that blocks: four threads is still four outstanding calls
    /// into somebody else's library.
    static constexpr size_t MinAuthenticationThreads = 4;

    /// A connection whose authentication cannot even be queued is refused. Failing closed is the
    /// only safe reading of "we could not check".
    ///
    /// How many queued authentications one thread is worth.
    ///
    /// The queue exists to hold a burst, so the only defensible depth is what the threads behind it
    /// can actually drain within MaxQueueWait - anything deeper is guaranteed to go stale and be
    /// refused after the broker has already spent a socket, a session object and the client's
    /// password sitting in memory on it. Measured on 2026-09-06: eight threads drained about ten
    /// thousand connections a second, so one thread is worth some 1250/s, and at a five-second
    /// deadline that is a little over six thousand. Rounded to a power of two.
    ///
    /// Scaled by the thread count rather than fixed, because the drain rate is: a fixed number is
    /// either too small on a large machine or a standing invitation to hold tens of thousands of
    /// half-made sessions on a small one. This is also why it is not a setting - the value that
    /// makes sense is one nobody can work out by hand, and it follows the hardware for free.
    ///
    /// The history says the same thing twice: 256 refused a thousand clients arriving together,
    /// and 8192 refused 3627 of a 20000-session persistent run while the broker had the capacity
    /// to serve every one of them and nowhere to put them.
    static constexpr size_t QueuedAuthenticationsPerThread = 8192;

    /// And no deeper than this, however many threads there are.
    ///
    /// The scaling above answers "how much can be drained in time"; this answers "how much has
    /// ever arrived". No run has yet delivered more than some tens of thousands of connections a
    /// second - 50000/s is the highest ever seen here - so a queue past 65536 is depth nobody has
    /// tested and nothing has needed, bought at the price of that many held sockets, session
    /// objects and passwords in memory. On sixteen threads the formula alone would ask for 131072.
    static constexpr size_t MaxQueuedAuthentications = 65536;

    /// How long a request may sit in that queue before it is refused unasked.
    ///
    /// This is what the small queue used to provide: with a store that has hung, clients are told
    /// "not available" instead of waiting for a thread that will not come free. Saying it directly
    /// is better than saying it through a queue length, which also refused legitimate bursts.
    static constexpr std::chrono::seconds MaxQueueWait {5};

    sptk::SynchronizedQueue<PendingAuth> m_authentications;
    std::atomic_size_t                   m_queuedAuthentications {0};

    /// QueuedAuthenticationsPerThread times the number of authentication threads, fixed when they
    /// are started. The floor covers the window before that, when nothing is queued anyway.
    std::atomic_size_t                   m_maxQueuedAuthentications {
        std::min<size_t>(MinAuthenticationThreads * QueuedAuthenticationsPerThread, MaxQueuedAuthentications)};
    std::atomic<uint64_t>                m_refusedAuthentications {0};
    std::vector<std::thread>             m_authenticationThreads;

    /// One AclGroup per group name, shared by every session in it - which is what makes the cache
    /// worth having. Written only when a connection meets a group for the first time.
    std::shared_mutex                                     m_groupsLock;
    std::unordered_map<std::string, std::shared_ptr<AclGroup>> m_groups;
};

} // namespace xmq
