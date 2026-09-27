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
 * @file TopicGuard.cpp
 * @brief Topic permissions, in the smallest form that shows the shape of them.
 *
 * Every client whose username begins with the same prefix is one group, and a group may use the
 * topics under its own name. A real one asks a directory for group membership and reads a rule
 * table; the structure around those two calls is what this is for, and it is the structure that
 * decides what authorization costs.
 *
 *     "settings": { "separator": "-", "root": "site" }
 *
 * so that `ops-alice` and `ops-bob` are both group `ops`, and both may publish to and subscribe to
 * `site/ops/...` and nothing else.
 *
 * The two calls are deliberately unlike each other.
 *
 * **resolve_group() runs once per connection and may block.** It is where a directory is asked.
 * Nothing here needs to block, but the point is that it could, and that a slow one costs the
 * connecting client its own latency and no one else's.
 *
 * **authorize() is asked about a group and a topic, never about a client.** The broker caches
 * every answer under that pair, so a thousand clients in one group publishing to one topic ask
 * once between them. Write rules that depend on the individual client and that sharing is gone -
 * the broker would have to ask again for every client, which is the difference between a hash
 * lookup and a call into a shared library on every message.
 *
 * It also abstains rather than denying whenever it can, for the same reason the authentication
 * sample does: an extension that denies what it was never meant to judge takes a broker down on
 * the day it is installed.
 */

#include "extension/XmqExtension.h"

#include <string>

namespace {

class TopicGuard : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        m_separator = setting("separator", "-").front();
        m_root = setting("root", "site");
        return true;
    }

    std::string resolveGroup(const xmq_auth_request& request) override
    {
        // Once per connection, on a thread that is allowed to wait. A directory-backed extension
        // asks LDAP here.
        const auto username = std::string(xmq::view(request.username));
        const auto separator = username.find(m_separator);
        return separator == std::string::npos ? std::string {} : username.substr(0, separator);
    }

    xmq_acl_decision authorize(const std::string_view group, const std::string_view topic,
                               xmq_acl_action /*action*/) override
    {
        // Asked once per group and topic, then cached by the broker. Must not block: read what
        // start() prepared and return.
        if (group.empty())
        {
            return XMQ_ACL_NOT_HANDLED; // nobody this extension knows; not its place to refuse
        }

        const auto owned = m_root + "/" + std::string(group) + "/";
        return topic.starts_with(owned) ? XMQ_ACL_ALLOW : XMQ_ACL_DENY;
    }

private:
    char        m_separator {'-'};
    std::string m_root {"site"};
};

} // namespace

XMQ_DEFINE_EXTENSION(TopicGuard, "topic-guard", "1.0", XMQ_CAP_AUTHORIZER)
