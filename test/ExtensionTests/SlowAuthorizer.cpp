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
 * @file SlowAuthorizer.cpp
 * @brief A test-only authorizer that answers slowly and never twice the same way.
 *
 * **Not a sample, and not to be copied.** It breaks the one rule authorize() has, which is not to
 * block, and it lives here rather than in examples/ for exactly that reason: the samples exist to
 * be copied, and one carrying test scaffolding teaches the wrong thing.
 *
 * It exists to hold open the window between "the extension was asked" and "the answer was cached".
 * The broker asks outside its lock and inserts under it, so a rule change landing in between used
 * to leave an answer taken under the old rules in a map that had just been emptied - an access
 * that was revoked and went on working. Reproducing that needs the call to take long enough for
 * the invalidation to land inside it, which is what `delay_ms` is for.
 *
 * The first call is answered ALLOW and every one after it DENY. That is what lets a test tell an
 * answer served from the broker's cache from one the extension was asked for again, with no way
 * to see inside the extension at all.
 */

#include "extension/XmqExtension.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

class SlowAuthorizer : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;

    bool start() override
    {
        m_delay = std::chrono::milliseconds(std::stoi(setting("delay_ms", "0")));
        return true;
    }

    std::string resolveGroup(const xmq_auth_request& request) override
    {
        return std::string(xmq::view(request.username));
    }

    xmq_acl_decision authorize(std::string_view /*group*/, std::string_view /*topic*/,
                               xmq_acl_action /*action*/) override
    {
        std::this_thread::sleep_for(m_delay);
        return m_calls.fetch_add(1) == 0 ? XMQ_ACL_ALLOW : XMQ_ACL_DENY;
    }

private:
    std::chrono::milliseconds m_delay {0};
    std::atomic<size_t>       m_calls {0};
};

} // namespace

XMQ_DEFINE_EXTENSION(SlowAuthorizer, "slow-authorizer", "1.0", XMQ_CAP_AUTHORIZER)
