/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE — sample extension                   ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This sample is placed in the public domain, or under CC0 1.0 where that is  ║
║  not possible. Copy it into your own extension, closed or open, without      ║
║  attribution or obligation — it exists to be copied. Note that this differs  ║
║  from the broker itself, which is under the Mozilla Public License 2.0: a    ║
║  file copied out of the broker would carry that licence with it, and this    ║
║  one deliberately does not.                                                  ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

/**
 * @file EventLog.cpp
 * @brief What an XMQ extension looks like, in full.
 *
 * It writes every broker event to a file. That is small enough to read in one sitting and real
 * enough to be useful: an SNMP agent or an alerting extension is this, with the file replaced by a
 * socket.
 *
 * Configure it in xmq_server.conf:
 *
 *     "extensions": [
 *       {
 *         "name": "event-log",
 *         "library": "/usr/local/lib/xmq/libxmq_event_log.so",
 *         "enabled": true,
 *         "settings": { "file": "/var/log/xmq/events.log" }
 *       }
 *     ]
 *
 * Two things about it are not decoration. onEvent() runs on a thread the broker keeps for
 * extensions, never on one carrying messages, so the file write here cannot slow delivery down -
 * at worst the broker drops events and says so. And the counter reported at stop() exists because
 * an extension that quietly does nothing is worse than one that fails: it lets you see, in the
 * broker's own log, that it saw what it should have seen.
 *
 */

#include "extension/XmqExtension.h"

#include <chrono>
#include <fstream>
#include <string>

namespace {

const char* eventName(const xmq_event_type type)
{
    switch (type)
    {
        case XMQ_EVENT_CLIENT_CONNECTED:
            return "connected";
        case XMQ_EVENT_CLIENT_DISCONNECTED:
            return "disconnected";
        case XMQ_EVENT_SUBSCRIBED:
            return "subscribed";
        case XMQ_EVENT_UNSUBSCRIBED:
            return "unsubscribed";
        case XMQ_EVENT_ERROR:
            return "error";
        case XMQ_EVENT_PUBLISHED:
            return "published";
    }
    return "unknown";
}

/// Microseconds since the epoch as a readable stamp, because a log nobody can read is a file.
std::string timestamp(const uint64_t microseconds)
{
    const auto point = std::chrono::system_clock::time_point(std::chrono::microseconds(microseconds));
    return std::format("{:%F %T}", std::chrono::floor<std::chrono::milliseconds>(point));
}

class EventLog : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        m_path = setting("file", "xmq_events.log");
        m_file.open(m_path, std::ios::out | std::ios::app);
        if (!m_file.is_open())
        {
            // Returning false is how an extension declines to run. The broker logs it, unloads
            // this extension and carries on serving clients - which is the right outcome for an
            // event log that cannot open its file.
            log(XMQ_LOG_ERROR, "cannot open " + m_path);
            return false;
        }
        // Asked for here and nowhere else: values are captured as the event is queued, on the
        // broker's message threads, so a name asked for later cannot reach back for what was never
        // taken. Only what is asked for is captured, which is why this is a request and not a
        // field of the event - the broker publishing one more fact costs no ABI version and no
        // rebuild of this. An older broker, or one that does not know the name, simply answers
        // nothing and the line below is left out.
        wantEventAttributes("remote_address");

        log(XMQ_LOG_INFO, "writing broker events to " + m_path);

        return true;
    }

    /**
     * @brief The settings changed while the broker runs.
     *
     * Only what changed is acted on: a reload that renamed nothing must not lose the file handle
     * and the lines buffered behind it for the sake of opening the same path again.
     */
    bool reload() override
    {
        const auto path = setting("file", "xmq_events.log");
        if (path == m_path)
        {
            return true;
        }

        std::ofstream opened(path, std::ios::out | std::ios::app);
        if (!opened.is_open())
        {
            // Refused rather than half-applied: the old file is still open and still being written
            // to, which is a state that can be reasoned about. Losing both would not be.
            log(XMQ_LOG_ERROR, "cannot open " + path + ", still writing to " + m_path);
            return false;
        }

        m_file.flush();
        m_file.close();
        m_file = std::move(opened);
        m_path = path;
        log(XMQ_LOG_INFO, "now writing broker events to " + m_path);
        return true;
    }

    bool stop() override
    {
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
        log(XMQ_LOG_INFO, std::format("wrote {} events to {}", m_written, m_path));
        return true;
    }

    void onEvent(const xmq_event& event) override
    {
        if (!m_file.is_open())
        {
            return;
        }

        m_file << timestamp(event.timestamp_us) << ' ' << eventName(event.type)
               << " client=" << xmq::view(event.client_id);

        if (const auto username = xmq::view(event.username); !username.empty())
        {
            m_file << " user=" << username;
        }
        if (const auto address = eventAttribute(event, "remote_address"); !address.empty())
        {
            m_file << " from=" << address;
        }

        // An error carries what it was in three attributes that are always there - they are what
        // the event says, not extra facts about it, so they need no wantEventAttributes().
        if (event.type == XMQ_EVENT_ERROR)
        {
            m_file << " subject=" << eventAttribute(event, "error_subject")
                   << " reason=" << eventAttribute(event, "error_reason")
                   << " message=\"" << eventAttribute(event, "error_message") << '"';
        }
        if (const auto topic = xmq::view(event.topic); !topic.empty())
        {
            m_file << " topic=" << topic << " qos=" << static_cast<int>(event.qos);
        }
        if (event.type == XMQ_EVENT_PUBLISHED)
        {
            m_file << " bytes=" << event.payload_size << (event.retain != 0 ? " retained" : "");
        }
        m_file << '\n';

        // Flushed per event on purpose: an event log is read while the broker is still running,
        // and this thread exists so that the cost of doing so lands here and nowhere else.
        m_file.flush();
        ++m_written;
    }

private:
    std::string   m_path;
    std::ofstream m_file;
    uint64_t      m_written {0};
};

} // namespace

const xmq_setting eventLogSettings[] {
    {.name = "file",
     .label = "Log file",
     .description = "Where broker events are written, one line each",
     .type = XMQ_SETTING_STRING,
     .default_value = "xmq_events.log",
     .choices = nullptr,
     .required = 0}};

XMQ_DEFINE_EXTENSION_DESCRIBED(EventLog, "event-log", "1.1", XMQ_CAP_OBSERVER,
                               "Writes every broker event to a file, one line each",
                               eventLogSettings)
