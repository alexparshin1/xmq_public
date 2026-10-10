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

#include "common/AtomicSharedPtr.h"
#include "Session.h"
#include <atomic>
#include <memory>
#include <sptk5/cutils>
#include <thread>

namespace xmq::client {

/**
 * @brief Generic MQTT client.
 */
class MqttClient
{
public:
    /**
     * @brief Constructor.
     * @param logEngine         Logger.
     * @param logPrefix         Log message prefix.
     * @param bindAddress       Bind to the local interface IP address.
     */
    explicit MqttClient(const std::shared_ptr<sptk::LogEngine>& logEngine = {},
                        const std::string&                      logPrefix = "",
                        sptk::String                            bindAddress = "");

    /**
     * @brief Destructor.
     */
    virtual ~MqttClient();

    /**
     * @brief Connect the client to a server host.
     * @param host              Server host.
     * @param credentials       Session credentials.
     * @param connectParameters Optional las will info.
     * @param protocolVersion   MQTT protocol version.
     * @param messageProperties Optional connect message messageProperties, MQTT5 only.
     * @param sslKeys           SSL keys if SSL connection is requested.
     * @return Success or the ReasonCode.
     */
    ReasonCode connect(const sptk::Host&                     host,
                       const ConnectCredentials&             credentials,
                       const ConnectParameters&              connectParameters,
                       ProtocolVersion                       protocolVersion = ProtocolVersion::MqttV5,
                       const SMessageProperties&             messageProperties = {},
                       const std::shared_ptr<sptk::SSLKeys>& sslKeys = {})
    {
        return connectInternal(host, credentials, connectParameters, protocolVersion, messageProperties,
                               sslKeys, nullptr);
    }

    /**
     * @brief Connect the client to a server host.
     * @param host              Server host.
     * @param credentials       Session credentials.
     * @param connectParameters Optional las will info.
     * @param protocolVersion   MQTT protocol version.
     * @param messageProperties Optional connect message messageProperties, MQTT5 only.
     * @param sslKeys           SSL keys if SSL connection is requested.
     * @param onConnectCompleted The callback called upon completion of the connection.
     * @return Success or the ReasonCode.
     */
    ReasonCode connectAsync(const sptk::Host&                     host,
                            const ConnectCredentials&             credentials,
                            const ConnectParameters&              connectParameters,
                            ProtocolVersion                       protocolVersion,
                            const SMessageProperties&             messageProperties,
                            const std::shared_ptr<sptk::SSLKeys>& sslKeys,
                            const MessageCallback&                onConnectCompleted)
    {
        if (!onConnectCompleted)
        {
            throw std::invalid_argument("onConnectCompleted can't be null");
        }
        return connectInternal(host, credentials, connectParameters, protocolVersion, messageProperties,
                               sslKeys, onConnectCompleted);
    }

    /**
     * @return Client ID.
     */
    [[nodiscard]] const std::string& getClientId() const;

    /**
     * @return Log prefix.
     */
    [[nodiscard]] std::string prefix() const
    {
        if (const auto session = m_session.load())
        {
            return session->prefix();
        }
        return "";
    }

    /**
     * @brief Disconnect client.
     * Sends the Disconnect message to the server and waits for response and/or
     * connection termination.
     */
    void disconnect() const;

    /**
     * @brief Terminates connection to the server.
     */
    void hangup() const;

    /**
     * @return True if the client is connected to the server.
     */
    bool isConnected() const;

    /**
     * @brief Enables or disables the keep-alive mechanism for the MQTT connection.
     * @param enable True to enable the keep-alive, false to disable it.
     *
     * This method controls the keep-alive mechanism for the MQTT connection.
     * When enabled, the client will periodically send PING messages to the server
     * to maintain the connection and verify that the connection is still alive.
     * This is useful for detecting broken connections early.
     */
    void enableKeepAlive(bool enable) const;

    /**
     * @brief Registers a callback to handle received MQTT messages.
     * @param messageCallback The callback function that will be invoked whenever
     * a message is received.
     */
    void onMessage(PublishMessageCallback messageCallback);

    /**
     * @brief Registers a callback to handle received ACK messages.
     * @param messageCallback The callback function that will be invoked whenever
     * an ACK message is received.
     */
    void onAck(MessageCallback messageCallback);

    /**
     * @brief Registers a callback to be invoked when the client disconnects.
     * @param messageCallback The callback function to handle the disconnect event.
     */
    void onDisconnect(MessageCallback messageCallback);

    /**
     * @brief Registers a callback to be invoked when a reconnect has succeeded.
     *
     * A client that was asked to reconnect comes back on its own, and this is how a caller learns
     * that it did: what it held on the connection - a subscription, above all - has to be taken
     * again, and only the caller knows what that was.
     */
    void onReconnect(std::function<void()> reconnectCallback);

    /**
     * @brief Subscribe to destination.
     * @param destination       Destination.
     * @param properties        Subscribe properties.
     */
    void subscribe(std::string_view destination, const SMessageProperties& properties = {});

    /**
     * @brief Subscribe to destination.
     * @param destination       Destination.
     * @param properties        Subscribe properties.
     */
    void subscribe(const Destination& destination, const SMessageProperties& properties = {});

    /**
     * @brief Subscribe to destinations.
     * @param destinations      Destinations.
     * @param properties        Subscribe properties.
    */
    MessageId subscribe(const Destinations& destinations, const SMessageProperties& properties = {});

    /**
     * @brief Unsubscribe from destination.
     * @param destination       Destination.
     */
    MessageId unsubscribe(const Destination& destination);

    /**
     * @brief Unsubscribe from destinations.
     * @param destinations      Destinations.
     */
    MessageId unsubscribe(const Destinations& destinations);

    /**
     * @brief Publish a message.
     * @param destination         Destination (topic name).
     * @param payload                Message data.
     * @param qos                 QoS.
     * @param properties          Message properties (MQTT5 only).
     * @param retain              The retain message flag.
     * @param duplicate           Duplicate message flag.
     */
    MessageId publish(const Topic* destination, const sptk::Buffer& payload,
                      Qos qos = Qos::Qos1, const SMessageProperties& properties = {},
                      bool retain = false, bool duplicate = false);

    /**
     * @brief Publish a message (minimal version).
     * @param destination       Destination (topic name).
     * @param payload              Message data.
     * @param qos               QoS.
     * @param retain            Flag: retain message.
     */
    MessageId publish(const std::string& destination, const std::string& payload,
                      Qos qos = Qos::Qos1, bool retain = false);

    /**
     * @brief Publish a message.
     * @param publishMessage    Publish message.
     */
    MessageId publish(const PublishMessage& publishMessage);

    /**
     * @brief Send ping.
     */
    MessageId ping();

    /**
     * @brief Simple method for waiting until the internal send/receive queue is empty.
     * @param timeout           Flush timeout. The default is 10 seconds.
     */
    void flush(std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;

    /**
     * @brief Retrieves the index associated with this MQTT client.
     * @return The client index as an integer.
     */
    size_t getClientIndex() const;

    /**
     * @brief Sets the index associated with this MQTT client.
     * @return The client index as an integer.
     */
    void setClientIndex(size_t clientIndex);

    /**
     * @brief Get logger.
     * @return copy of the logger shared pointer.
     */
    [[nodiscard]] std::shared_ptr<sptk::Logger> getLogger() const;

    /**
     * @brief Get MQTT protocol version.
     * @return MQTT protocol version.
     */
    [[nodiscard]] ProtocolVersion protocolVersion() const;

    /**
     * @brief Get the topic by the topic name.
     * @param topic             Topic name.
     * @return topic.
     */
    static const Topic* getTopic(std::string_view topic);

    /**
     * @brief How many QoS 1 and 2 publications may be unacknowledged at once on this connection -
     *        the server's Receive Maximum when it announced a lower one.
     */
    [[nodiscard]] uint16_t inflightLimit() const;

private:
    /**
     * @brief Keeps callbacks from accessing a client after its destruction begins.
     * @details Sessions retain this state even when they outlive the client or are replaced.
     */
    struct MessageCallbackState
    {
        std::atomic_bool   closing {false}; ///< Destruction has disabled access to the client.
        std::atomic_size_t active {0};      ///< Callbacks that may still access the client.
    };

    std::shared_ptr<MessageCallbackState>                      m_messageCallbackState = std::make_shared<MessageCallbackState>(); ///< Lifetime guard shared with every session.
    std::shared_ptr<sptk::LogEngine>                           m_logEngine;               ///< Log engine.
    bool                                                       m_hasCallerLogPrefix {false}; ///< Caller named this client; connect() must not relabel it.
    std::shared_ptr<sptk::Logger>                              m_logger;                  ///< Logger (set once in the constructor, then read-only).
    AtomicSharedPtr<Session>                                   m_session {nullptr};       ///< Client session.
    sptk::String                                               m_bindAddress;             ///< Bind to local interface IP address.
    AtomicSharedPtr<const PublishMessageCallback>              m_onMessage {nullptr};     ///< Callback for received Publish messages.
    mutable std::atomic_size_t                                 m_activePublishCallbacks {0}; ///< Publish callbacks currently executing; lets onMessage({}) drain in-flight calls before returning.
    AtomicSharedPtr<const MessageCallback>                     m_onAck {nullptr};         ///< Callback for received ACKs.
    AtomicSharedPtr<const MessageCallback>                     m_onDisconnect {nullptr};  ///< Optional callback called for disconnection.
    AtomicSharedPtr<const std::function<void()>>               m_onReconnect {nullptr};   ///< Called when a reconnect has worked.
    std::atomic_size_t                                         m_clientIndex = 0;         ///< Client index.
    std::atomic_size_t                                         m_publishReceiveCount = 0; ///< Received messages count.
    std::atomic_size_t                                         m_publishSentCount = 0;    ///< Sent messages count.

    /// What the last connect was given, so that a reconnect can be the same connection.
    struct CConnectArguments
    {
        sptk::Host                     m_host;
        ConnectCredentials             m_credentials;
        ConnectParameters              m_parameters;
        ProtocolVersion                m_protocolVersion {ProtocolVersion::MqttV5};
        SMessageProperties             m_messageProperties;
        std::shared_ptr<sptk::SSLKeys> m_sslKeys;
        bool                           m_valid {false};
    };

    /// True from the moment the caller disconnects the client on purpose until the next connect:
    /// what must not be brought back is the disconnection somebody asked for.
    mutable std::atomic_bool m_disconnectRequested {false};

    /// True while a reconnect is being attempted, so that a disconnection caused by an attempt does
    /// not start a second one at the same time.
    std::atomic_bool m_reconnecting {false};

    /// The arguments of the connection in force. A shared pointer to a constant because they are
    /// written by whoever connects and read by the reconnect worker on a thread of its own - and
    /// because sptk::Host can be constructed but not assigned, so a copy is made once and only its
    /// pointer travels afterwards. The mutex guards the pointer.
    mutable std::mutex                       m_connectArgumentsMutex;
    std::shared_ptr<const CConnectArguments> m_connectArguments;

    std::jthread m_reconnectWorker; ///< Bringing the client back, when it was asked to come back.

    /**
     * @brief Connect the client to a server host.
     * @param host              Server host.
     * @param credentials       Session credentials.
     * @param connectParameters Optional las will info.
     * @param protocolVersion   MQTT protocol version.
     * @param messageProperties Optional connect message messageProperties, MQTT5 only.
     * @param sslKeys           SSL keys if SSL connection is requested.
     * @param onConnectCompleted The callback called upon completion of connection.
     * @return Success or the failure reason code.
     */
    ReasonCode connectInternal(const sptk::Host&                     host,
                               const ConnectCredentials&             credentials,
                               const ConnectParameters&              connectParameters,
                               ProtocolVersion                       protocolVersion,
                               const SMessageProperties&             messageProperties,
                               const std::shared_ptr<sptk::SSLKeys>& sslKeys,
                               const MessageCallback&                onConnectCompleted);

    /**
     * @return copy of the session shared pointer.
     */
    SSession getSession() const;

    /**
     * @brief Notice a disconnection the caller did not ask for, and start bringing the client back.
     * @param message           The disconnection message.
     */
    void onUnexpectedDisconnect(const SMessage& message);

    /**
     * @brief Bring the client back: wait, ask where to go, connect, and say so when it worked.
     *
     * Runs on a thread of its own, because it is called from the disconnection of a session, which
     * happens on that session's receive thread.
     */
    void startReconnect();

    /**
     * @brief Preview the incoming message.
     * @param message           The incoming message.
     */
    void previewMessage(const SMessage& message);

    /**
     * @brief Sends a message using the MQTT client.
     * @param message           The message to be sent.
     * @param retain            Indicates whether the broker should retain the message.
     */
    MessageId sendMessage(const SMessage& message, bool retain);

    /**
     * @brief Build and send a Publish message from raw payload bytes.
     * @remark Shared implementation for the public publish() overloads. Takes the payload as a
     *         string_view so callers never materialize an intermediate Buffer (the PublishMessage
     *         constructor copies the bytes once into its own storage).
     */
    MessageId publishPayload(const Topic* destination, std::string_view payload, Qos qos,
                             const SMessageProperties& properties, bool retain, bool duplicate);

    /**
     * @brief Execute onMessage callback if it is defined.
     * @param message Message.
     */
    void executeOnPublishMessage(const SMessage& message) const;

    /**
     * @brief Execute onDisconnect callback if it is defined.
     * @param message Message.
     */
    void executeOnDisconnect(const SMessage& message) const;

    /**
     * @brief Execute onAck callback if it is defined.
     * @param message Message.
     */
    void executeOnAck(const SMessage& message) const;

    /**
     * @brief Closes the MQTT session.
     */
    void closeSession();
};

using SMqttClient = std::shared_ptr<MqttClient>;

} // namespace xmq::client
