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

#include "Bridge.h"

#include "server/Server.h"
#include "server/Settings/Settings.h"
#include "server/Subscription/Subscription.h"

#include <chrono>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

// How long to wait before trying an unreachable remote broker again. Brief, because two brokers
// bridged to each other cannot both be started first: the one that starts first will always fail
// its initial attempt and should pick the connection up promptly once its peer is listening.
constexpr auto ReconnectInterval = chrono::milliseconds(500);

// A remote that is down tends to stay down for a while, and retrying twice a second buys nothing
// but a failure logged twice a second. The wait doubles after each failed attempt up to this cap,
// and returns to ReconnectInterval as soon as a connection succeeds, so a bridge still recovers
// promptly once the remote comes back.
constexpr auto MaxReconnectInterval = chrono::milliseconds(30000);

constexpr uint16_t DefaultMqttPort = 1883;

// A topic is carried inbound when its direction says so. Absent direction means both ways, which
// matches how the bridge's own mode is written.
bool isInboundTopic(const CBridgeTopic& topic)
{
    const auto direction = topic.m_direction.asString();
    return direction.empty() || direction == "in" || direction == "inout";
}

bool isOutboundTopic(const CBridgeTopic& topic)
{
    const auto direction = topic.m_direction.asString();
    return direction.empty() || direction == "out" || direction == "inout";
}

} // namespace

Bridge::Bridge(Server* server, const CBridge& settings)
    : m_server(server)
    , m_settings(settings)
    , m_remoteNodeName(settings.m_node_name.asString())
    // The client logs through the server's log engine, labelled with the bridge configuration
    // entry it belongs to - node name and id, because several bridges may share a client id and
    // the shipped sample configuration does exactly that. Without a log engine the client is
    // silent, and the bridge's own view of what it sent - the one thing that separates "never
    // sent" from "sent and lost" - is invisible.
    , m_mqttClient(server->getLogEngine(),
                   "(bridge " + settings.m_node_name.asString() +
                       (settings.m_id.isNull() ? String("") : "#" + settings.m_id.asString()) + ") ")
{
    // The configured client id, or one derived the way the cluster names its node connections.
    // Either way it must be stable across reconnects: a non-clean bridge session on the remote
    // broker is keyed by it.
    m_clientId = m_settings.m_client_id.isNull() ? "" : m_settings.m_client_id.asString();
    if (m_clientId.empty())
    {
        m_clientId = "bridge_" + m_server->getNodeName() + "_" + m_remoteNodeName;
    }
}

Bridge::~Bridge()
{
    stop();
}

void Bridge::start()
{
    {
        const scoped_lock lock(m_mutex);
        if (m_thread.joinable())
        {
            return;
        }
        m_stopping = false;
    }

    m_thread = thread([this]
                      {
                          run();
                      });
}

void Bridge::stop()
{
    {
        const scoped_lock lock(m_mutex);
        if (m_stopping)
        {
            return;
        }
        m_stopping = true;
    }
    m_wakeup.notify_all();

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    if (m_mqttClient.isConnected())
    {
        // Before the disconnect, while there is still a connection to send it on. A failure here
        // must not stop the rest of the shutdown - the remote may already have gone away, which
        // is precisely one of the reasons a bridge is being stopped.
        if (!m_remoteSubscriptions.empty())
        {
            try
            {
                m_mqttClient.unsubscribe(m_remoteSubscriptions);
            }
            catch (const Exception& exception)
            {
                m_server->logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                                     [this, &exception]
                                     {
                                         return format("Bridge to {} could not unsubscribe on stop: {}",
                                                       m_remoteNodeName, exception.what());
                                     });
            }
        }
        m_mqttClient.disconnect();
    }
    m_remoteSubscriptions.clear();

    // Take the outbound subscriber back out of the local subscriptions. Its callback captures
    // this Bridge, so a subscription still holding it after the bridge goes away would deliver
    // into a destroyed object. Done after the thread is joined, so nothing re-subscribes behind
    // this loop.
    for (const auto& subscription: m_outboundSubscriptions)
    {
        subscription->removeSubscriptionClient(m_outboundSubscriber.get());
    }
    m_outboundSubscriptions.clear();
    m_outboundSubscriber.reset();
}

bool Bridge::isConnected() const
{
    return m_mqttClient.isConnected();
}

void Bridge::run()
{
    auto retryInterval = ReconnectInterval;

    while (true)
    {
        {
            unique_lock lock(m_mutex);
            if (m_stopping)
            {
                return;
            }
        }

        if (!m_mqttClient.isConnected() && !connectAndSubscribe())
        {
            {
                unique_lock lock(m_mutex);
                m_wakeup.wait_for(lock, retryInterval, [this]
                                  {
                                      return m_stopping;
                                  });
            }
            retryInterval = std::min(retryInterval * 2, MaxReconnectInterval);
            continue;
        }

        // Connected, so the next outage starts from the short interval again.
        retryInterval = ReconnectInterval;

        // Connected: poll for a drop, and re-establish the subscriptions when one happens. The
        // remote may have restarted, in which case it remembers nothing about this bridge.
        unique_lock lock(m_mutex);
        m_wakeup.wait_for(lock, ReconnectInterval, [this]
                          {
                              return m_stopping;
                          });
    }
}

bool Bridge::connectAndSubscribe()
{
    auto remoteHost = make_unique<Host>(m_settings.m_host_port.asString());
    if (remoteHost->port() == 0)
    {
        remoteHost = make_unique<Host>(m_settings.m_host_port.asString(), DefaultMqttPort);
    }

    const ConnectCredentials credentials(m_clientId,
                                         m_settings.m_username.asString(),
                                         m_settings.m_password.asString());

    client::ConnectParameters connectParameters;
    connectParameters.m_cleanSession = m_settings.m_clean_session.isNull() || m_settings.m_clean_session.asBool();

    // Tells the remote that this connection is a bridge from this node. It marks the session as a
    // bridge origin, which is what stops it echoing back messages that originated elsewhere.
    const auto messageProperties = make_shared<MessageProperties>();
    messageProperties->setUserProperty("origin-node", m_server->getNodeName());

    shared_ptr<SSLKeys> sslKeys;
    if (!m_settings.m_encrypted.isNull() && m_settings.m_encrypted.asBool())
    {
        const auto& keysData = m_settings.m_ssl_keys;

        // This node's own pair when the bridge names none: what identifies this server to the
        // remote is the same certificate whatever the link, and asking for it to be configured
        // per bridge only invites the copies to drift apart.
        const auto [nodeCertificate, nodeKey] = Settings::nodeKeyFiles();
        const auto certificateFile = keysData.m_certfile.asString().empty()
                                         ? nodeCertificate.string()
                                         : keysData.m_certfile.asString();
        const auto privateKeyFile = keysData.m_keyfile.asString().empty()
                                        ? nodeKey.string()
                                        : keysData.m_keyfile.asString();

        // The certificates of the nodes this one trusts, when the bridge names no authority of
        // its own. Between brokers there is usually no certificate authority at all: each side
        // holds the other's certificate, and a self-signed certificate vouches for itself.
        auto authorityFile = keysData.m_cafile.asString();
        if (authorityFile.empty())
        {
            authorityFile = Settings::buildPeerCertificateBundle().string();
        }

        // Verification follows the configured depth, the same way the listeners' does - except
        // that a bridge with trusted peers and nothing said about depth verifies, because an
        // administrator who imported the remote's certificate meant it to be checked. Left
        // unverified, as this was regardless of configuration, the link is protected against
        // listening and not at all against something answering in the remote broker's place.
        auto verifyDepth = keysData.m_verify_depth.asInteger();
        if (verifyDepth == 0 && !authorityFile.empty() && keysData.m_cafile.asString().empty())
        {
            verifyDepth = 1;
        }
        const auto verifyMode = verifyDepth ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;

        sslKeys = make_shared<SSLKeys>(privateKeyFile.c_str(), certificateFile.c_str(), "",
                                       authorityFile.c_str(), verifyMode, verifyDepth);
    }

    try
    {
        m_mqttClient.onMessage([this](const SPublishMessage& message)
                               {
                                   acceptRemoteMessage(message);
                               });

        if (const auto reasonCode = m_mqttClient.connect(*remoteHost, credentials, connectParameters,
                                                         ProtocolVersion::MqttV5, messageProperties, sslKeys);
            reasonCode != ReasonCode::Success)
        {
            return false;
        }
    }
    catch (const Exception& exception)
    {
        m_server->logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                             [this, &exception]
                             {
                                 return format("Bridge to {} not connected yet: {}", m_remoteNodeName, exception.what());
                             });
        return false;
    }

    size_t subscribedCount = 0;
    m_remoteSubscriptions.clear();
    for (const auto& topic: m_settings.m_topics)
    {
        if (!isInboundTopic(topic))
        {
            continue;
        }

        const auto pattern = topic.m_pattern.asString();
        if (pattern.empty())
        {
            continue;
        }

        const auto qos = topic.m_qos.isNull() ? Qos::Qos1 : static_cast<Qos>(topic.m_qos.asInteger());

        // No Local: the remote must not send back what this bridge itself published there. It is
        // the standard form of what Mosquitto's try_private asks for, it is enforced by the
        // remote rather than relying on anything this side marks, and it works against any MQTT 5
        // broker. Retain As Published: without it a retained message loses its retain flag on the
        // way across, and arrives as an ordinary one.
        auto remoteOptions = SubscriptionOptions(qos);
        remoteOptions.setNoLocal(true);
        remoteOptions.setRetainAsPublished(true);

        m_mqttClient.subscribe(Destination(client::MqttClient::getTopic(pattern), remoteOptions));
        // Remembered so stop() can take them off the remote again. A bridge session is not clean,
        // so the remote keeps whatever this bridge subscribed to: left behind, the filters of a
        // bridge that has been reconfigured or removed stay live, and the remote goes on queueing
        // for a session nothing will read.
        m_remoteSubscriptions.emplace_back(client::MqttClient::getTopic(pattern), remoteOptions);
        ++subscribedCount;
    }

    subscribeOutboundTopics();

    m_server->logMessage(LogSubject::ServerConnections, LogPriority::Info,
                         [this, &remoteHost, subscribedCount]
                         {
                             return format("Bridge to {} ({}) connected, {} inbound topic(s).",
                                           m_remoteNodeName, remoteHost->toString(), subscribedCount);
                         });

    return true;
}

void Bridge::subscribeOutboundTopics()
{
    if (const auto mode = m_settings.m_mode.asString();
        mode != "out" && mode != "inout")
    {
        return;
    }

    // Subscribe once, on the first successful connection: the local subscription is independent
    // of the remote connection and must not be duplicated when the bridge reconnects.
    if (m_outboundSubscriber)
    {
        return;
    }

    m_outboundSubscriber = make_shared<BridgeSubscriber>("bridge_out_" + m_clientId, m_remoteNodeName,
                                                         [this](const SPublishMessage& message)
                                                         {
                                                             forwardToRemote(message);
                                                         });

    size_t outboundCount = 0;
    for (const auto& topic: m_settings.m_topics)
    {
        if (!isOutboundTopic(topic))
        {
            continue;
        }

        const auto pattern = topic.m_pattern.asString();
        if (pattern.empty())
        {
            continue;
        }

        const auto qos = topic.m_qos.isNull() ? Qos::Qos1 : static_cast<Qos>(topic.m_qos.asInteger());

        // Retain As Published on the local side too, so a retained message keeps its retain flag
        // when it is handed to this subscriber and goes out to the remote still retained. No
        // Local has nothing to do here: it compares the publisher against the subscriber's own
        // client id, and this subscriber never publishes.
        auto localOptions = SubscriptionOptions(qos);
        localOptions.setRetainAsPublished(true);

        auto subscription = m_server->getSubscriptionManager()->subscribe(m_server->getTopic(pattern), m_outboundSubscriber,
                                                                          qos, 0, localOptions);
        if (subscription)
        {
            m_outboundSubscriptions.push_back(std::move(subscription));
        }
        ++outboundCount;
    }

    m_server->logMessage(LogSubject::ServerConnections, LogPriority::Info,
                         [this, outboundCount]
                         {
                             return format("Bridge to {} forwarding {} outbound topic(s).",
                                           m_remoteNodeName, outboundCount);
                         });
}

void Bridge::forwardToRemote(const SPublishMessage& message)
{
    if (!message || !m_mqttClient.isConnected())
    {
        return;
    }

    try
    {
        m_mqttClient.publish(*message);
    }
    catch (const Exception& exception)
    {
        m_server->logMessage(LogSubject::ServerConnections, LogPriority::Error,
                             [this, &exception]
                             {
                                 return format("Bridge to {} could not forward a message: {}",
                                               m_remoteNodeName, exception.what());
                             });
    }
}

void Bridge::acceptRemoteMessage(const SPublishMessage& message) const
{
    if (!message)
    {
        return;
    }

    // The message was decoded by the bridge's own client, which keeps a topic manager of its own,
    // so its destination is a different Topic object than the server's one of the same name.
    // Subscriptions are keyed by topic pointer: left as it arrived, the message is filed under a
    // subscription beside the one local subscribers hold. Live delivery does not notice - the
    // whole group for the topic is visited - but the retained message would be kept where nobody
    // ever looks for it, and the topic counted twice.
    message->setDestination(m_server->getTopic(message->destination()->fullName()));

    // Stamping the source node is what marks this as bridged traffic: Subscription::deliverTo()
    // refuses to hand a message with a source node to a bridge subscription, so it cannot travel
    // back the way it came.
    message->setSender(m_remoteNodeName);
    message->setSourceNode(m_remoteNodeName);

    m_server->logMessage(LogSubject::ServerConnections, LogPriority::Debug,
                         [this, &message]
                         {
                             return format("Bridge from {} carried a message on {}{}.",
                                           m_remoteNodeName, message->destination()->fullName(),
                                           message->isRetain() ? " (retained)" : "");
                         });

    m_server->publishMessage(message);
}
