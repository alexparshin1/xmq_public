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

#include "base/SharedStringTable.h"

#include "common/AtomicSharedPtr.h"
#include <atomic>

#include "../Extensions/ExtensionHost.h"
#include "../Subscription/Subscription.h"
#include "ClientSessionSerial.h"
#include "common/BaseClientSession.h"

namespace xmq {

class Server;

/**
 * @brief Client subscription.
 */
struct ClientSubscription
{
    SSubscription       subscription;           ///< Subscription.
    Qos                 qos {Qos::Qos0};        ///< Subscription QOS.
    SubscriptionOptions subscriptionOptions {}; ///< Subscription options.
};

using SubscriptionMap = std::map<std::string_view, ClientSubscription, std::less<>>;

class XMQ_EXPORT ClientSessionData
    : public BaseClientSession
{
public:
    /**
     * @brief Constructor.
     * Creates the client connection object and makes it persistent
     * @param server                    Server.
     * @param connectMessageParameters         Session parameters.
     * @param connectMessageProperties  Connect message properties.
     */
    explicit ClientSessionData(Server*                   server,
                               SConnectMessageParameters connectMessageParameters = std::make_shared<ConnectMessageParameters>(),
                               SMessageProperties        connectMessageProperties = {});

    /**
     * @brief Destructor.
     */
    ~ClientSessionData() noexcept override;

    /**
     * @brief Get the connection protocol version.
     * @return Protocol version.
     */
    [[nodiscard]] ProtocolVersion getProtocolVersion() const
    {
        return m_protocolVersion.load(std::memory_order_relaxed);
    }

    /**
     * @brief Set the connection protocol version.
     * @return Protocol version.
     */
    void setProtocolVersion(const ProtocolVersion protocolVersion) const
    {
        m_protocolVersion.store(protocolVersion, std::memory_order_relaxed);
    }

    /**
     * @brief Get string representation of the object.
     * @return String representation of the object.
     */
    std::string toString() const;

    /**
     * @return Set session's client ID.
     */
    void setClientId(std::string_view clientId);

    /**
     * @return Session's client ID.
     */
    [[nodiscard]] const std::string& getClientId() const override;

    [[nodiscard]] std::string_view getClientIdUnlocked() const override;

    /**
     * @return Username.
     */
    [[nodiscard]] std::string_view getUsername() const override
    {
        return m_username == nullptr ? std::string_view() : std::string_view(*m_username);
    }

    /**
     * @return Log messages prefix.
     */
    [[nodiscard]] std::string prefix() const override
    {
        const std::shared_lock lock(m_mutex);
        // Built here rather than kept: it is the client id and a space, and keeping it cost 32 bytes
        // in every session to save an allocation that the caller makes anyway - prefix() has always
        // returned by value.
        return m_clientId.empty() ? std::string() : m_clientId + " ";
    }

    [[nodiscard]] bool isCleanSession() const override;

    /**
     * @brief Take from the CONNECT what outlives it.
     *
     * The parameters object is not kept. What a session needs after the CONNACK is the protocol
     * version, the clean-session flag, the keep-alive and the last will; the client id and the
     * username it already owns copies of; and the password it does not need at all, so holding the
     * object would keep the credentials in memory for the life of the session. The caller must hold
     * the session lock.
     */
    void applyConnectParametersUnlocked(const SConnectMessageParameters& connectMessageParameters);

    /**
     * @return The last will, or nothing when there is none or it has been discarded.
     */
    [[nodiscard]] SLastWillInfo getLastWill() const
    {
        return m_lastWill.load(std::memory_order_acquire);
    }
    [[nodiscard]] SMessageProperties getConnectProperties() const;
    [[nodiscard]] SMessageProperties getConnectPropertiesUnlocked() const;
    void                                    setConnectProperties(const SMessageProperties& properties);
    void                                    setConnectPropertiesUnlocked(const SMessageProperties& properties);
    [[nodiscard]] const std::string&        getNodeName() const;
    void                                    setNodeName(const std::string& nodeName);

    /**
     * @return Session's keep alive seconds.
     */
    [[nodiscard]] uint16_t getKeepAliveSeconds() const;

    /**
     * @brief Get last will message.
     * @return Last will message.
     */
    [[nodiscard]] std::shared_ptr<PublishMessage> getLastWillMessage(const STopicManager& topicManager) const;

    /**
     * @brief Discard the session's last will.
     *
     * A client that disconnects normally must not produce a last will, so the will is dropped
     * when DISCONNECT arrives rather than only suppressed at that moment: the socket's FIN can
     * be reported as a separate event afterwards, and that would otherwise re-flag the session
     * as a connection loss and publish a will the client explicitly avoided.
     */
    void discardLastWill();

    void                   setConnectMessageReceived(bool received);
    [[nodiscard]] bool     connectMessageReceived() const;
    [[nodiscard]] uint32_t getSubscriptionId(const SubscribeMessage* subscriptionMessage) const;

    /**
     * @brief Check if the connection is subscribed to the topic.
     * @param topic             Topic name.
     * @return True if the connection is subscribed to the topic.
     */
    bool isSubscribed(std::string_view topic) const;

    /**
     * @brief Unsubscribe all subscriptions.
     */
    void unsubscribeAll();

    /**
     * @brief Unsubscribe from the topic.
     * @param topic             Topic name.
     */
    void unsubscribe(std::string_view topic);

    /**
     * @brief Get XMQ server.
     * @return XMQ server.
     */
    [[nodiscard]] Server& server() const
    {
        const std::shared_lock lock(m_mutex);
        return *m_xmqServer;
    }

    /**
     * @brief The permissions of the group this session's client belongs to.
     *
     * Established once, while the client is being authenticated, and not changed afterwards - so
     * it is read without the session lock, and a permission check on the publish path costs an
     * atomic load and a hash lookup rather than contending with everything else the session does.
     *
     * Null on every broker with no authorizing extension, which is what the checks test first.
     */
    [[nodiscard]] AclGroup* aclGroup() const noexcept
    {
        return m_aclGroup.load(std::memory_order_acquire);
    }

    /// Called once, on the authentication thread, before the client is told it may proceed.
    ///
    /// Two members for one thing, deliberately: the shared_ptr owns, and the plain pointer is what
    /// the publish path reads. An atomic load of a shared_ptr is not a pointer load - libstdc++
    /// takes a spinlock out of a global pool for it - and this is read once per message by every
    /// session at once, which is exactly the shape that turns that pool into contention.
    void setAclGroup(std::shared_ptr<AclGroup> group)
    {
        m_aclGroupOwner = std::move(group);
        m_aclGroup.store(m_aclGroupOwner.get(), std::memory_order_release);
    }

    /**
     * @brief Get the map of the session's subscriptions.
     * Should be called from locked context.
     * @return Map of the session's subscriptions.
     */
    const SubscriptionMap& getSubscribedTo() const
    {
        const std::shared_lock lock(m_mutex);
        return m_subscribedTo;
    }

    /**
     * @brief Count the session's subscriptions.
     * @return Number of subscriptions.
     */
    [[nodiscard]] size_t subscriptionCount() const
    {
        const std::shared_lock lock(m_mutex);
        return m_subscribedTo.size();
    }

    /**
     * @brief Add the subscription to the client.
     * @param subscription          Subscription.
     * @param qos                   Subscription QOS.
     * @param subscriptionOptions   Subscription options.
     */
    void subscribedTo(const std::shared_ptr<Subscription>& subscription, Qos qos, SubscriptionOptions subscriptionOptions) override;

    /**
     * @brief Remove the subscription from the client.
     * @param subscription      Subscription.
     */
    void unsubscribedFrom(const Subscription& subscription) override;

protected:
    /**
     * @brief Remove all subscriptions from the client (unlocked).
     */
    void unsubscribeAllUnlocked();

    /**
     * @brief Get the map of the session's subscriptions.
     * Should be called from locked context.
     * @return Map of the session's subscriptions.
     */
    const SubscriptionMap& getSubscribedToUnlocked() const;

    /**
     * @brief Get XMQ server.
     * @return XMQ server.
     */
    [[nodiscard]] Server& serverUnlocked() const
    {
        return *m_xmqServer;
    }

    virtual void queueMessageDelivery(SMessageDispatch& messageDispatch) const = 0;
    virtual void restoreMessageDelivery(SMessageDispatch& messageDispatch) const = 0;

private:
    Server*     m_xmqServer; ///< XMQ server

    /// Client ID and username, held by the session rather than read out of the parameters.
    ///
    /// getClientId() returns const std::string& and getUsername() a string_view - the
    /// ISubscriptionClient interface fixes both - so callers hold references into whatever these
    /// are read from, while a reconnect replaces the parameters object wholesale. Their values do
    /// not change across a reconnect; only the object carrying them does. Copies owned by the
    /// session outlive the swap, so the references stay good.
    std::string m_clientId;
    /// The account, in the shared table: one fleet of devices signs in as one user, and a session
    /// holding the name itself would hold a copy of it per connection.
    const std::string* m_username {nullptr};

    /// The group whose permissions apply to this session. See aclGroup() and setAclGroup().
    std::shared_ptr<AclGroup> m_aclGroupOwner;
    std::atomic<AclGroup*>    m_aclGroup {nullptr};

    /// Session parameters and properties; both replaced on CONNECT while other threads read them.
    /// Atomic so a reader is handed an owning copy and the object it is reading cannot be
    /// destroyed underneath it - a plain shared_ptr here let a reconnect free the parameters
    /// while the receive thread was still reading strings out of them.
    /// What the CONNECT leaves behind. The parameters object itself is let go with the message.
    mutable std::atomic<ProtocolVersion>      m_protocolVersion {ProtocolVersion::MqttV31};
    std::atomic_bool                          m_cleanSession {false};
    std::atomic<uint16_t>                     m_keepAliveSeconds {0};
    AtomicSharedPtr<LastWillInfo>             m_lastWill;
    AtomicSharedPtr<IMessageProperties>        m_connectProperties;
    std::atomic_bool          m_connectMessageReceived {false}; ///< Flag indicating that session received the first Connect message.
    SubscriptionMap           m_subscribedTo;                   ///< The subscriptions of this session.
    const std::string*        m_nodeName {nullptr};             ///< Node name, in the shared table.
};

} // namespace xmq
