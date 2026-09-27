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

#include "common/ISubscriptionClient.h"

#include <functional>
#include <string>

namespace xmq {

/**
 * @brief Local subscriber that hands matching messages to an outbound bridge.
 *
 * The outbound half of a bridge is an ordinary local subscription: this subscribes to the
 * configured patterns on its own server and forwards whatever matches to the remote broker. It is
 * not a network client and never appears as a session - the server delivers to it directly.
 *
 * @remarks bridgeOrigin() returns the remote node's name, and that is what keeps traffic from
 * circulating. Subscription::deliverTo() refuses to hand a message that carries a source node to a
 * subscription whose client has a bridge origin, so anything that arrived over a bridge is never
 * forwarded back out over one.
 */
class BridgeSubscriber final
    : public ISubscriptionClient
{
public:
    using ForwardCallback = std::function<void(const SPublishMessage& message)>;

    /**
     * @brief Constructor.
     * @param clientId          Identifies this subscriber in logs and subscription bookkeeping.
     * @param remoteNodeName    Name of the node this bridge connects to.
     * @param forward           Invoked for every message that should go to the remote broker.
     */
    BridgeSubscriber(std::string clientId, std::string remoteNodeName, ForwardCallback forward)
        : m_clientId(std::move(clientId))
        , m_remoteNodeName(std::move(remoteNodeName))
        , m_forward(std::move(forward))
    {
    }

    [[nodiscard]] const std::string& getClientId() const override
    {
        return m_clientId;
    }

    [[nodiscard]] std::string_view getClientIdUnlocked() const override
    {
        return m_clientId;
    }

    [[nodiscard]] std::string_view getUsername() const override
    {
        return m_username;
    }

    [[nodiscard]] RecordId recordId() const override
    {
        return 0;
    }

    [[nodiscard]] SStorage storage() const override
    {
        // A bridge subscription is derived from configuration and rebuilt on every start, so it
        // is never persisted.
        return {};
    }

    void subscribedTo(const std::shared_ptr<Subscription>&, Qos, SubscriptionOptions) override
    {
    }

    MessageId postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet& subscriptionIds, bool retain) override;

    void onMessage(MessageCallback) override
    {
    }

    /**
     * @brief The node this subscription bridges to.
     *
     * Non-empty by design: it marks the subscription as a bridge, which is what stops bridged
     * traffic being forwarded back the way it came.
     */
    [[nodiscard]] std::string bridgeOrigin() const override
    {
        return m_remoteNodeName;
    }

    void setBridgeOrigin(const std::string&) override
    {
    }

private:
    std::string     m_clientId;
    std::string     m_remoteNodeName;
    std::string     m_username {"bridge"};
    ForwardCallback m_forward;
};

using SBridgeSubscriber = std::shared_ptr<BridgeSubscriber>;

} // namespace xmq
