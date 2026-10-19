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

#include "ClusterInvitations.h"
#include "server/Server.h"

#include <sptk5/xdoc/Node.h>
#include <sptk5/cutils>

using namespace std;
using namespace sptk;

namespace xmq::cluster {

namespace {

/// The fields an invitation carries, as the inviter wrote them.
String fieldOf(const xdoc::Document& document, const char* name)
{
    const auto node = document.root()->findFirst(name);
    if (!node || node->getValue().isNull())
    {
        return {};
    }
    return node->getValue().asString();
}

} // namespace

void ClusterInvitations::offer(const String& payload, const String& inviterName)
{
    Invitation invitation;
    invitation.m_inviterName = inviterName;
    invitation.m_receivedAt = DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true);

    try
    {
        xdoc::Document document;
        document.load(payload);

        invitation.m_inviterHostPort = fieldOf(document, "inviter_host_port");
        invitation.m_redisUri = fieldOf(document, "redis_uri");
        invitation.m_databaseUri = fieldOf(document, "database_uri");
        invitation.m_xmqVersion = fieldOf(document, "xmq_version");

        if (invitation.m_inviterHostPort.empty() ||
            invitation.m_redisUri.empty() || invitation.m_databaseUri.empty())
        {
            invitation.m_rejectReason = "The invitation is missing a field; nothing was changed.";
        }
        else
        {
            check(invitation);
        }
    }
    catch (const Exception& e)
    {
        invitation.m_rejectReason = String("The invitation does not parse: ") + e.what();
    }

    const scoped_lock lock(m_mutex);
    m_invitation = std::move(invitation);
}

void ClusterInvitations::check(Invitation& invitation)
{
    if (invitation.m_xmqVersion != Server::getVersion())
    {
        invitation.m_rejectReason = format("The inviting node runs XMQ {}, this node {} - a node joins a cluster of its own version.",
                                           invitation.m_xmqVersion.c_str(), Server::getVersion().c_str());
    }
}

optional<ClusterInvitations::Invitation> ClusterInvitations::current() const
{
    const scoped_lock lock(m_mutex);
    return m_invitation;
}

void ClusterInvitations::decline()
{
    const scoped_lock lock(m_mutex);
    m_invitation.reset();
}

String ClusterInvitations::refusalPayload(const String& reason)
{
    return String(R"({"refused":true,"reason":")") + reason + R"("})";
}

} // namespace xmq::cluster
