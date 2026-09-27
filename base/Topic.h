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

#include "MapSelection.h"
#include "base/xmq.h"
#include <memory>
#include <sptk5/cutils>
#include <vector>

#include <sptk5/threads/ReadWriteLock.h>

namespace xmq {

class Subscription;

/**
 * @brief Subscription topic.
 */
class XMQ_EXPORT Topic final
{
    friend class TopicManager;

public:
    /**
     * @brief Copy assignment operator
     * @param other             Topic to copy
     */
    Topic& operator=(const Topic& other) = delete;

    /**
     * @brief Move assignment operator
     * @param other             Topic to move
     */
    Topic& operator=(Topic&& other) noexcept = delete;

    /**
     * @brief Get topic name
     * @return Topic name
     */
    [[nodiscard]] std::string_view name() const
    {
        return {m_buffer.c_str(), m_buffer.size()};
    }

    /**
     * @brief Get the topic path as a vector of strings
     * @return topic path as a vector of strings
     */
    [[nodiscard]] const std::vector<std::string_view>& path() const
    {
        return m_path;
    }

    /**
     * @brief Get the topic's full name, including the prefix "$share/" (if any)
     * @return
     */
    [[nodiscard]] std::string_view fullName() const
    {
        return m_fullName;
    }

    /**
     * @brief Get the topic's share group name. If topic doesn't start from "$share/" - it's empty.
     * @return
     */
    [[nodiscard]] std::string_view shareGroupName() const
    {
        return m_shareGroupName;
    }

    /**
     * @brief Get topic string representation
     * @return topic string representation
     */
    [[nodiscard]] std::string_view toString() const
    {
        return {m_buffer.c_str(), m_buffer.size()};
    }

    /**
     * @brief Get topic name size
     * @return
     */
    [[nodiscard]] size_t size() const
    {
        return m_buffer.size();
    }

    /**
     * @brief Check if the topic is the wildcard topic
     * @return true if the topic is the wildcard topic
     */
    [[nodiscard]] bool isWildcard() const
    {
        return m_isWildcard;
    }

    /**
     * @brief Check if the topic is the system topic (can't be published by clients).
     * @return true if the topic is the system topic.
     */
    [[nodiscard]] bool isSystem() const
    {
        return m_isSystem;
    }

    /**
     * @brief Check if the topic is the cluster topic (can be published only by user 'cluster').
     * @return true if the topic is the cluster topic.
     */
    [[nodiscard]] bool isCluster() const
    {
        return m_isCluster;
    }

    /**
     * @brief Check if the topic is shared
     */
    [[nodiscard]] bool isShared() const
    {
        return m_isShared;
    }

    [[nodiscard]] bool empty() const
    {
        return m_buffer.empty();
    }

    /**
     * @brief Compare two topics
     * @param other         The other topic
     * @return
     */
    bool operator==(const Topic& other) const
    {
        return m_buffer == other.m_buffer;
    }

    /**
     * @brief Set the subscription to the topic
     * @param subscription      Subscription
     */
    void setSubscription(const std::shared_ptr<Subscription>& subscription)
    {
        std::unique_lock lock(m_mutex);
        m_subscription = subscription;
    }

    /**
     * @brief Get topic's subscription
     * @return Topic's subscription
     */
    std::weak_ptr<Subscription> getSubscription() const
    {
        std::shared_lock lock(m_mutex);
        return m_subscription;
    }

protected:
    /**
     * @brief Protected constructor
     * @remarks Use the create() method to construct a topic.
     * @param topicName             Topic name
     */
    explicit Topic(std::string_view topicName);
    void analyzeDollarTopic(const char*& elementStart);

private:
    mutable std::shared_mutex     m_mutex;              ///< Mutex that protects topic data
    sptk::Buffer                  m_buffer;             ///< Topic name buffer
    std::vector<std::string_view> m_path;               ///< Topic path as the name split to vector of strings
    std::string_view              m_fullName;           ///< Topic name
    std::string_view              m_shareGroupName;     ///< Share group name
    std::weak_ptr<Subscription>   m_subscription;       ///< Subscription (single topics only)
    bool                          m_isWildcard {false}; ///< True if the topic is the wildcard topic
    bool                          m_isShared {false};   ///< True if the topic is the shared topic (queue)
    bool                          m_isSystem {false};   ///< True if the topic is the system topic
    bool                          m_isCluster {false};  ///< True if the topic is the cluster topic
    void                          initPath();           ///< Initialize the topic after its name is set
};

using STopic = std::shared_ptr<Topic>;

class XMQ_EXPORT TopicManager final
{
public:
    /**
     * @brief Topic factory
     * @remarks Topics created with the topic factory are cached.
     * @param topicName         Topic name
     * @return The created or cached topic
     */
    const Topic* getTopic(std::string_view topicName);

    /**
     * @brief Topic factory
     * @remarks Topics created with the topic factory are cached.
     * @param topic             Topic
     * @return The created or cached topic
     */
    const Topic* getTopic(const Topic& topic);

    size_t size();

    std::shared_ptr<Topic> get(std::string_view topicName);

    static const Topic* emptyTopic(); ///< Empty topic ("").

private:
    using Map = XMQ_MAP_TYPE<std::string_view, std::shared_ptr<Topic>>;
    sptk::ReadWriteMutex m_factoryMutex; ///< Topic factory mutex
    Map                  m_cachedTopics; ///< Cached topics, created by topic factory
};

using STopicManager = std::shared_ptr<TopicManager>;

} // namespace xmq
