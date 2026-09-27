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

namespace xmq {
class Server;

namespace cluster {

class Cluster;

/**
 * @brief Subscription client that receives all cluster-related messages.
 */
class SubscriptionClient final
    : public ISubscriptionClient
    , public std::enable_shared_from_this<SubscriptionClient>
{
public:
    /**
     * Constructor.
     * @param server            XMQ server.
     */
    explicit SubscriptionClient(Server* server);

    [[nodiscard]] const std::string& getClientId() const override;
    [[nodiscard]] std::string_view   getClientIdUnlocked() const override;
    [[nodiscard]] std::string_view   getUsername() const override;

    void subscribedTo(const std::shared_ptr<Subscription>&, Qos, SubscriptionOptions) override
    {
    }

    [[nodiscard]] int64_t  recordId() const override;
    [[nodiscard]] SStorage storage() const override;

    /**
     * Receive messages sent to $CLUSTER/request/# to bridge connections.
     * @param message           Cluster request message.
     * @param qos               QOS, ignored.
     * @param subscriptionIds   Subscription IDs, ignored.
     * @param retain            Retain flag, ignored.
     */
    MessageId postMessage(const SMessage& message, Qos qos, const SubscriptionIdSet& subscriptionIds, bool retain) override;

    void onMessage(MessageCallback messageCallback) override;

    void executeMessageCallback(const SMessage& message) const;

    [[nodiscard]] std::string bridgeOrigin() const override;

    void setBridgeOrigin(const std::string&) override
    {
    }

private:
    mutable std::mutex m_mutex;
    Server*            m_server;
    const Topic*       m_requestAttachNode {nullptr};
    MessageCallback    m_messageCallback;
};

using SSubscriptionClient = std::shared_ptr<SubscriptionClient>;

} // namespace cluster
} // namespace xmq
