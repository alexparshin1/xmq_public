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
#include "GenericProtocol.h"
#include "ISubscriptionClient.h"
#include "SessionCounters.h"
#include "TopicAliases.h"
#include "base/MessageDispatch.h"
#include "base/MessageQueue.h"

#include <atomic>
#include <memory>

namespace xmq {

struct XMQ_EXPORT SendReceiveResult
{
    size_t bytes {0};
    size_t anyMessages {0};
    size_t publishMessages {0};
    bool   noMoreMessages {true};
};

class XMQ_EXPORT BaseClientSession
    : public ISubscriptionClient
    , public SessionCounters
    , public TopicAliases
{
public:
    enum class SessionType : uint8_t
    {
        Client,
        Server
    };

    explicit BaseClientSession(SessionType sessionType)
        : m_sessionType(sessionType)
    {
    }

    ~BaseClientSession() override;

    virtual std::string prefix() const = 0;

    virtual bool isCleanSession() const = 0;

    virtual SendReceiveResult receiveMessages() = 0;
    virtual void              receivedAck(const MessageId& messageId, Message::Type messageType) = 0;

    /**
   * @brief Directly send the message to the client.
   * @param messageDispatch            Message delivery.
   */
    virtual void sendMessage(SMessageDispatch& messageDispatch) = 0;

    /**
   * @brief Write the message to the binary buffer.
   * @param messageDispatch            Message delivery.
   * @return binary buffer.
   */
    virtual const sptk::Buffer& writeMessageUnlocked(const MessageDispatch& messageDispatch);

    void touchLastClientMessageTimestamp();

    [[nodiscard]] sptk::DateTime::time_point getLastClientMessageTimestamp() const;

    [[nodiscard]] const GenericProtocol& protocol() const
    {
        // A session that has not been given one answers with an empty protocol rather than with
        // nothing: the connect path asks for the version before it sets the protocol, to see
        // whether it has to change at all.
        static const GenericProtocol none;
        return m_protocol == nullptr ? none : *m_protocol;
    }

    virtual void clearProtocol();
    virtual void setProtocol(const GenericProtocol& protocol);
    void         setSocket(const sptk::STCPSocket& socket);

    /**
     * @brief The session's socket, as an owning pointer.
     *
     * Returned by value, and deliberately not as a reference into the member: a disconnect
     * on another thread calls setSocket(nullptr), and a caller holding a reference (or a raw
     * pointer taken from a temporary) would be left pointing at a destroyed socket. The
     * returned shared_ptr keeps it alive for as long as the caller holds it.
     *
     * @return the socket, or nullptr when the session has none.
     */
    [[nodiscard]] sptk::STCPSocket getSocket() const
    {
        return m_socket.load(std::memory_order_acquire);
    }

    /**
     * @return The queue of messages in flight, or nothing when this session has never needed one.
     *
     * Most sessions never do: a publisher, a subscriber at QoS 0, a connection that only sits there.
     * Callers that only read the queue check for nothing and do nothing; callers that put a message
     * in it use inflightQueueUnlocked(), which makes it.
     */
    [[nodiscard]] const SMessageQueue& getInflightQueueUnlocked() const
    {
        return m_inflightQueue;
    }

    /**
     * @return The queue of messages in flight, made now if this is the first message that needs it.
     */
    const SMessageQueue& inflightQueueUnlocked() const;

    [[nodiscard]] const SMessageQueue& getInflightQueue() const
    {
        std::unique_lock lock(m_mutex);
        return m_inflightQueue;
    }

    std::vector<SMessageDispatch> enqueuedMessages() const;
    void                          setMaxInflightMessages(uint16_t maxInflightMessages) const;

    /**
     * @brief How many QoS 1 and 2 messages may be unacknowledged at once - 0 asked for the default.
     */
    [[nodiscard]] uint16_t inflightLimit() const
    {
        return m_maxInflightMessages.load(std::memory_order_relaxed);
    }

    virtual SMessage decodePacket(Packet&&, uint64_t)
    {
        return {};
    }

    virtual void handleMessage(const SMessage&)
    {
    }

    void applyConnectProperties(const IMessageProperties& properties,
                                ReasonCode&              responseCode);

    /**
   * @brief Get maximum packet size.
   * @return maximum packet size.
   */
    [[nodiscard]] size_t getMaximumPacketSize() const
    {
        return m_maximumPacketSize;
    }

    /**
   * @brief Set the maximum packet size.
   * @param maximumPacketSize     Maximum packet size.
   */
    void setMaximumPacketSize(const size_t maximumPacketSize)
    {
        m_maximumPacketSize = maximumPacketSize;
    }

    sptk::Buffer& writeBuffer()
    {
        return m_writeBuffer;
    }

    virtual bool isConnected() const
    {
        // The socket is taken once, into an owning copy. Testing the member and then
        // dereferencing it separately let a concurrent setSocket(nullptr) drop the last
        // reference in between, leaving active() to lock a mutex inside a destroyed socket.
        const auto socket = getSocket();
        return socket && socket->active();
    }

    bool canRead(const size_t byteCount) const
    {
        const auto socket = getSocket();
        return socket && socket->socketBytes() >= byteCount;
    }

    virtual void closeSession();

    void onMessage(MessageCallback messageCallback) override;

    void executeMessageCallback(const SMessage& message) const
    {
        if (const auto callback = m_messageCallback.load())
        {
            (*callback)(message);
        }
    }

    [[nodiscard]] std::string bridgeOrigin() const override;

    void setBridgeOrigin(const std::string&) override
    {
    }

    SessionType getSessionType() const
    {
        return m_sessionType;
    }

protected:
    mutable std::shared_mutex m_mutex;

    virtual void autoAck(const Message* message) = 0;
    /**
     * @brief Hand a message the queue has released to whoever owns the session.
     *
     * A virtual rather than a callback held per session: the queue is built when it is first needed,
     * and remembering how to build it would cost every session a std::function whether or not it
     * ever gets one.
     */
    virtual void forwardMessage(const SMessageDispatch& messageDispatch) = 0;

    /**
     * @brief Say how large the queue may grow, before there is a queue.
     */
    void         setInflightLimit(uint16_t maxInflightMessages);
    MessageId    nextSendMessageId();

    virtual bool queueSendMessage(const MessageDispatch&)
    {
        throw sptk::Exception("Not implemented");
    }

private:

    mutable std::shared_ptr<MessageQueue>   m_inflightQueue;                   ///< Made on first use, from a const path as often as not.
    mutable std::atomic<uint16_t>           m_maxInflightMessages {0};         ///< What to build it with.
    /// The next packet id to send. A 56-byte shared_mutex used to guard this one counter, on an
    /// object the broker keeps one of per session, and the whole of what it guarded was an increment
    /// that skips zero - which an atomic does on its own.
    std::atomic<MessageId>                  m_nextMessageId {0};
    std::atomic<sptk::DateTime::time_point> m_lastClientMessageTimestamp;
    /// The session's socket. Atomic because a disconnect on one thread replaces or clears it
    /// while session threads are reading it: a plain shared_ptr read concurrent with a write
    /// is a data race, and the reader could be left with a pointer to a destroyed socket.
    AtomicSharedPtr<sptk::TCPSocket>        m_socket;
    /// The protocol this session speaks. A pointer, not a copy: there are three protocol objects in
    /// the process - one per MQTT version - and they are owned by a GenericProtocols that outlives
    /// every session. Copying one into each session meant 56 bytes and three atomic increments per
    /// connection, for three shared pointers that all sessions of that version share anyway.
    const GenericProtocol*                  m_protocol {nullptr};              ///< Session protocol
    sptk::Buffer                            m_writeBuffer {128};               ///< Session write buffer.
    std::atomic_size_t                      m_maximumPacketSize {0xFFFFFFFFU}; ///< Maximum MQTT packet size in the session.
    AtomicSharedPtr<const MessageCallback>  m_messageCallback {nullptr};       ///< Message callback retained by an executing receiver.
    SessionType                             m_sessionType;                     ///< If true then this is Session.
};


using SBaseClientSession = std::shared_ptr<BaseClientSession>; ///< A shared pointer to
                                                               ///< BaseClientSession
using WBaseClientSession = std::weak_ptr<BaseClientSession>;   ///< A weak pointer to BaseClientSession

} // namespace xmq
