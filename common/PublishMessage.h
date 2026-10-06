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

#include "base/LatencyTrace.h"
#include "base/Message.h"
#include "base/Topic.h"

#include <algorithm>
#include <string>
#include <vector>

namespace xmq {

class ClientSession;

/**
 * Generic Publish message - used as a base class for all Publish messages
 */
class XMQ_EXPORT PublishMessage : public Message
{
public:
    /**
     * @brief Constructor.
     */
    PublishMessage()
        : Message(Type::Publish)
    {
    }

    /**
     * @brief Destructor.
     */
    ~PublishMessage() override = default;

    /**
     * @brief Get message sender.
     * @return message sender.
     */
    [[nodiscard]] const std::string& getSender() const
    {
        return m_sender;
    }

    /**
     * @brief Set message sender.
     * @param sender Message sender.
     */
    void setSender(const std::string_view sender)
    {
        m_sender = sender;
    }

    /**
     * @brief Get message destination.
     * @return message destination.
     */
    [[nodiscard]] virtual const Topic* destination() const = 0;

    /**
     * @brief Set the message destination.
     * @param destination       Message destination.
     */
    virtual void setDestination(const Topic* destination) = 0;

    /**
     * @brief Get the message payload.
     * @return message payload pointer.
     */
    [[nodiscard]] virtual std::string_view payload() const = 0;

    /**
     * @brief Get the message payload pointer.
     * @return message payload pointer.
     */
    [[nodiscard]] virtual const uint8_t* payloadData() const = 0;

    /**
     * @brief Get message payload size.
     * @return message payload size.
     */
    [[nodiscard]] virtual uint32_t payloadSize() const = 0;
    /**
     * @brief Get the source node name.
     * @return Source node name.
     */
    [[nodiscard]] virtual const std::string& getSourceNode() const = 0;

    /**
     * @brief Set the source node name.
     * The source node name is set when the message is received from the other node.
     * @param sourceNode Source node name.
     */
    virtual void setSourceNode(std::string_view sourceNode) = 0;

    [[nodiscard]] LatencyTrace* asTrace() const
    {
        const auto data = this->payload();
        if (data.size() < sizeof(LatencyTrace)) { return nullptr; }
        const auto* trace = reinterpret_cast<const LatencyTrace*>(data.data());
        return trace->m_signature == 0x5115 ? const_cast<LatencyTrace*>(trace) : nullptr;
    }

    /**
     * @brief Was the message forwarded by another node of this broker's cluster?
     *
     * Such a message is delivered to this node's subscribers, but its retained part is not taken
     * from it: the originating node sends that separately, with the time it was made.
     */
    [[nodiscard]] bool isFromCluster() const
    {
        return m_fromCluster;
    }

    /**
     * @brief Mark the message as forwarded by another cluster node.
     */
    void setFromCluster()
    {
        m_fromCluster = true;
    }

    /**
     * @brief The shared subscriptions, as "$share/{ShareName}/{filter}", that the node a forwarded
     *        message came from assigned to this node.
     *
     * A shared subscription gets one recipient in the whole cluster, and the node the message
     * entered through chooses it. When it chooses a member on another node, it says so with the
     * message; that node then hands the message to one of its members of that subscription, and to
     * none of its members of any other - those were served where they were chosen.
     */
    /// The user property a forwarded message carries its assigned shared subscriptions in, one per
    /// subscription. Taken off again by the node that receives it, so no client ever sees it.
    static constexpr std::string_view ClusterShareProperty {"$xmq-cluster-share"};

    [[nodiscard]] const std::vector<std::string>& clusterShares() const
    {
        return m_clusterShares;
    }

    [[nodiscard]] bool isAssignedClusterShare(std::string_view share) const
    {
        return std::ranges::find(m_clusterShares, share) != m_clusterShares.end();
    }

    void setClusterShares(std::vector<std::string> shares)
    {
        m_clusterShares = std::move(shares);
    }

private:
    std::string              m_sender;              ///< Message sender.
    bool                     m_fromCluster {false}; ///< Forwarded by another cluster node.
    std::vector<std::string> m_clusterShares;       ///< Shared subscriptions assigned to this node, for a forwarded message.
};

using SPublishMessage = std::shared_ptr<PublishMessage>;

} // namespace xmq
