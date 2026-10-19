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

#pragma once

#include "base/xmq.h"

#include <mutex>
#include <optional>

namespace xmq {

class Settings;

namespace cluster {

/**
 * @brief The invitation a node holds, and the checks it must pass before it may accept.
 *
 * An invitation arrives as an MQTT publication to the node's own `cluster/invitation` topic,
 * made by the inviting node as the `cluster` user - the one account whose password is the same on
 * every node. The server is the subscriber: no client of the invited node is involved, and the
 * publication is not routed to any. One invitation at a time; a later one replaces the earlier.
 */
class XMQ_EXPORT ClusterInvitations
{
public:
    /// The invitation as the Cluster screen shows it, and what it would change.
    struct Invitation
    {
        sptk::String m_inviterName;      ///< Who invited, for people.
        sptk::String m_inviterHostPort;  ///< The address the reply and the links go to.
        sptk::String m_redisUri;         ///< The storage the cluster works against.
        sptk::String m_databaseUri;      ///< The accounts database every node shares.
        sptk::String m_xmqVersion;       ///< The inviter's version; a mismatch is a refusal.
        sptk::String m_receivedAt;       ///< ISO time it arrived, for the ten-minute lifetime.
        sptk::String m_rejectReason;     ///< Why the node may not accept; empty when it may.
    };

    /**
     * @brief Take an invitation from a publication to `cluster/invitation`.
     *
     * The payload is the JSON the inviter built. A payload that does not parse is kept with the
     * reason, so the administrator sees that an invitation arrived and what was wrong with it
     * rather than nothing at all.
     */
    void offer(const sptk::String& payload, const sptk::String& inviterName);

    /// The active invitation, if one is held.
    [[nodiscard]] std::optional<Invitation> current() const;

    /**
     * @brief Refuse it: the invitation is gone, and the inviter is told on cluster/refusal -
     * the caller passes the connection the reply is published over.
     */
    void decline();

    /**
     * @brief Accept it: apply the configuration the invitation offered and ask the service loop
     * to end the process for the restart that reads it.
     *
     * Returns an error text, or empty when the configuration is on disk and the restart is
     * requested. An invitation held with a reason cannot be accepted.
     */
    sptk::String accept();

    /// The reply the Decline sends, built here so the format has one owner: the refused node's
    /// GUID and the reason, as the inviter reads them from cluster/refusal.
    [[nodiscard]] static sptk::String refusalPayload(const sptk::String& nodeId, const sptk::String& reason);

    /// The invitation as the caller works with it, while it is held.
    [[nodiscard]] std::optional<Invitation> current();

private:
    /// What the invited node answers to, and whether the answer is a refusal.
    void check(Invitation& invitation);

    /// The node's own settings, for applying the accept; set once, by Cluster.
    friend class Cluster;
    void setSettings(std::shared_ptr<Settings> settings);

    mutable std::mutex m_mutex;
    std::optional<Invitation> m_invitation;
    std::shared_ptr<Settings> m_settings;
};

} // namespace cluster
} // namespace xmq
