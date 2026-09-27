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

#include "BridgeSubscriber.h"
#include "client/MqttClient.h"
#include "common/Destination.h"
#include "service/CBridge.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include <vector>

namespace xmq {

class Server;
class Subscription;

/**
 * @brief One bridge connection to another MQTT broker.
 *
 * A bridge works the way a Mosquitto bridge does: this server connects out to the remote broker
 * as an ordinary MQTT client. In "in" mode it subscribes there to the configured topic patterns
 * and republishes whatever arrives into its own subscription tree, so local subscribers see
 * remote traffic without knowing a second broker exists.
 *
 * Topics map one to one - the message keeps the name it was published under.
 *
 * Loops are prevented by the same mechanism the cluster uses. The connection carries an
 * "origin-node" user property, so the remote marks the session as a bridge origin, and messages
 * republished here carry the remote's name as their source node. A message that arrived over a
 * bridge is therefore never handed back to a bridge subscription.
 *
 * @remarks The remote broker may be unreachable when this server starts - it commonly is, since
 * brokers that bridge to each other cannot both start first. Connecting therefore runs on its own
 * thread and retries until it succeeds, rather than failing startup.
 */
class Bridge final
{
public:
    /**
     * @brief Constructor. Does not connect; call start().
     * @param server        Owning server.
     * @param settings      Bridge configuration entry.
     */
    Bridge(Server* server, const CBridge& settings);

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    /**
     * @brief Destructor. Stops the connection thread and disconnects.
     */
    ~Bridge();

    /**
     * @brief Begin connecting, and keep reconnecting for as long as the bridge exists.
     */
    void start();

    /**
     * @brief Stop reconnecting and drop the connection.
     */
    void stop();

    /**
     * @brief Whether the bridge is currently connected to the remote broker.
     */
    [[nodiscard]] bool isConnected() const;

    /**
     * @brief Name of the remote node, as configured.
     */
    [[nodiscard]] const std::string& remoteNodeName() const
    {
        return m_remoteNodeName;
    }

private:
    /**
     * @brief Connect and subscribe. Returns false if the remote could not be reached.
     */
    bool connectAndSubscribe();

    /**
     * @brief Republish a message received from the remote broker into the local server.
     */
    void acceptRemoteMessage(const SPublishMessage& message) const;

    /**
     * @brief Subscribe locally to the outbound patterns, so local traffic reaches the remote.
     */
    void subscribeOutboundTopics();

    /**
     * @brief Send a locally published message to the remote broker.
     */
    void forwardToRemote(const SPublishMessage& message);

    void run();

    mutable std::mutex      m_mutex;              ///< Mutex for thread safety.
    Server*                 m_server;             ///< XMQ server.
    CBridge                 m_settings;           ///< The bridge configuration.
    std::string             m_remoteNodeName;     ///< The name of the remote node, as configured.
    std::string             m_clientId;           ///< Derived; the bridge schema carries no client id.
    client::MqttClient      m_mqttClient;         ///< The MQTT client to connect to remote node.
    SBridgeSubscriber       m_outboundSubscriber; ///< Local subscription feeding the outbound direction.

    /// The local subscriptions the outbound subscriber was added to. Kept so stop() can take it
    /// out of them again: the subscriber's callback holds this Bridge, so leaving it registered
    /// past the bridge's lifetime would leave the subscription calling into a destroyed object.
    std::vector<std::shared_ptr<Subscription>> m_outboundSubscriptions;

    /// What this bridge subscribed to on the remote broker, so stop() can unsubscribe again.
    Destinations m_remoteSubscriptions;

    std::condition_variable m_wakeup;             ///< Condition variable for waking up the thread.
    bool                    m_stopping {false};   ///< Flag indicating whether the bridge is stopping.
    std::thread             m_thread;             ///< Thread for running the bridge.
};

using SBridge = std::shared_ptr<Bridge>;

} // namespace xmq
