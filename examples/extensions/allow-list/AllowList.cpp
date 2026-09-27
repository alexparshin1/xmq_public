/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE — sample extension                   ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This sample is placed in the public domain, or under CC0 1.0 where that is   ║
║  not possible. Copy it into your own extension, closed or open, without       ║
║  attribution or obligation — it exists to be copied. Note that this differs   ║
║  from the broker itself, which is under the Mozilla Public License 2.0.       ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

/**
 * @file AllowList.cpp
 * @brief An authentication extension, in the smallest form that is still honest.
 *
 * It admits client ids named in its configuration and abstains on everyone else. A real one asks a
 * directory server instead of reading a list; everything around that call is the same, which is
 * what the sample is for.
 *
 * It declares XMQ_CAP_AUTHENTICATOR and nothing else, so the broker never hands it an event. That
 * is not the broker being tidy - with no observer in its table there is nothing to call. An
 * extension is given exactly what it asked for and no more.
 *
 *     "extensions": [
 *       {
 *         "name": "allow-list",
 *         "library": "/usr/local/lib/xmq/libxmq_allow_list.so",
 *         "enabled": true,
 *         "settings": { "allow": "sensor-1, sensor-2, gateway", "lookup_ms": "0" }
 *       }
 *     ]
 *
 * Two things in here matter more than the list.
 *
 * **It answers NOT_HANDLED, not DENY, for a client it does not recognise.** This extension knows
 * the clients on its list and nothing whatever about anyone else. Denying them would lock out
 * every client the broker's own accounts - or the next extension in the chain - would have
 * admitted, which is how an authentication extension takes a broker down on the day it is
 * installed. Deny what you know to be wrong; abstain on what you do not know.
 *
 * **It is allowed to be slow.** authenticate() runs on a thread the broker keeps for
 * authentication, never on one carrying messages. `lookup_ms` stands in for a directory round trip
 * so the effect can be seen: set to 700, four clients connecting at once still finish in about
 * 700 ms, and traffic from every other client is untouched throughout.
 */

#include "extension/XmqExtension.h"

#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Splits "a, b, c" into its trimmed parts, without the empties. Written out rather than pulled
/// from a library, because an extension should need nothing but a standard library.
std::vector<std::string> splitOn(const std::string& text, const char separator)
{
    std::vector<std::string> parts;
    std::string              part;
    std::istringstream       stream(text);
    while (std::getline(stream, part, separator))
    {
        const auto first = part.find_first_not_of(" \t");
        const auto last = part.find_last_not_of(" \t");
        if (first != std::string::npos)
        {
            parts.push_back(part.substr(first, last - first + 1));
        }
    }
    return parts;
}

class AllowList : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        for (const auto& clientId: splitOn(setting("allow"), ','))
        {
            m_allowed.insert(clientId);
        }
        if (const auto lookup = setting("lookup_ms"); !lookup.empty())
        {
            m_lookup = std::chrono::milliseconds(std::stoi(lookup));
        }
        // Stands in for the directory being down. A real extension reaches this state by failing
        // to connect, not by configuration, but the answer it must then give is the same one.
        m_unreachable = setting("unreachable") == "1";

        if (m_allowed.empty())
        {
            // Refusing to start beats running: an authentication extension with an empty list
            // abstains on every client, which looks exactly like working and is not.
            log(XMQ_LOG_ERROR, "no 'allow' setting, so there is nobody this could admit");
            return false;
        }

        log(XMQ_LOG_INFO, "admitting " + std::to_string(m_allowed.size()) + " client id(s)");
        return true;
    }

    bool stop() override
    {
        log(XMQ_LOG_INFO, "admitted " + std::to_string(m_admitted) + " connection(s), abstained on " +
                              std::to_string(m_abstained));
        return true;
    }

    xmq_auth_decision authenticate(const xmq_auth_request& request) override
    {
        // Where a real extension calls its directory. Blocking here is expected: see the file
        // comment for what the broker does meanwhile, which is everything else.
        if (m_lookup.count() > 0)
        {
            std::this_thread::sleep_for(m_lookup);
        }

        // Say so, rather than abstaining or denying. Abstaining would hand the client to whoever
        // is next and make an outage look like somebody else's user; denying would tell a client
        // with perfectly good credentials that they are wrong.
        if (m_unreachable)
        {
            log(XMQ_LOG_ERROR, "directory unreachable");
            return XMQ_AUTH_SUBSYSTEM_ERROR;
        }

        const auto clientId = std::string(xmq::view(request.client_id));
        if (m_allowed.find(clientId) != m_allowed.end())
        {
            ++m_admitted;
            log(XMQ_LOG_INFO, "admitted " + clientId + (request.encrypted != 0 ? " over TLS" : ""));
            return XMQ_AUTH_ALLOW;
        }

        ++m_abstained;
        return XMQ_AUTH_NOT_HANDLED;
    }

private:
    std::set<std::string>     m_allowed;
    bool                      m_unreachable {false};
    std::chrono::milliseconds m_lookup {0};
    uint64_t                  m_admitted {0};
    uint64_t                  m_abstained {0};
};

} // namespace

XMQ_DEFINE_EXTENSION(AllowList, "allow-list", "1.0", XMQ_CAP_AUTHENTICATOR)
