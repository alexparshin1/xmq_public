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

#include "Subscription/SubscriptionManager.h"
#include "base/xmq.h"

#include <atomic>
#include <string>
#include <functional>

namespace xmq {

class XMQ_EXPORT SystemStatistics final
{
public:
    /// Queue depths, supplied by the broker: the statistics object has no access to the thread
    /// pools, and giving it one would tie it to the server's internals for three numbers.
    struct QueueDepths
    {
        size_t receive {0};
        size_t send {0};
        size_t delivery {0};
    };

    enum class SysTopicKind : uint8_t
    {
        BrokerLoadBytesReceived,
        BrokerLoadBytesSent,
        BrokerClientsConnected,
        BrokerClientsDisconnected,
        BrokerClientsMaximum,
        BrokerClientsTotal,
        BrokerMessagesReceived,
        BrokerMessagesSent,
        BrokerMessagesPublishDropped,
        BrokerMessagesPublishReceived,
        BrokerMessagesPublishSent,
        BrokerMessagesRetainedCount,
        BrokerMessagesStored,
        BrokerSubscriptionsCount,
        // Depth of the broker's own hand-off queues. Unlike the counters above, these say what the
        // broker is doing right now rather than what it has done: a queue that stands full is the
        // first sign it has begun falling behind, and it shows before the delay reaches latency.
        BrokerQueuesReceive,
        BrokerQueuesSend,
        BrokerQueuesDelivery,
        BrokerTime,
        BrokerUptime,
        BrokerVersion,
        Max
    };

    class Metric final
    {
    public:
        explicit Metric(const Topic* topic = nullptr);
        Metric(const STopicManager& topic_manager, SysTopicKind sysTopicKind, const std::string& topicPath);

        [[nodiscard]] uint64_t getValue() const
        {
            return m_value;
        }

        void setValue(const uint64_t value)
        {
            m_value = value;
            m_changed = true;
        }

        [[nodiscard]] uint64_t readValue()
        {
            m_changed = false;
            return m_value;
        }

        Metric& operator=(const uint64_t value)
        {
            m_value = value;
            m_changed = true;
            return *this;
        }

        Metric& operator+=(const uint64_t incr)
        {
            if (incr)
            {
                m_changed = true;
                m_value += incr;
            }
            return *this;
        }


        Metric& operator-=(const uint64_t decr)
        {
            if (decr)
            {
                m_changed = true;
                m_value -= decr;
            }
            return *this;
        }

        explicit operator uint64_t() const
        {
            return m_value;
        }

        [[nodiscard]] bool isChanged() const
        {
            return m_changed;
        }

        [[nodiscard]] const Topic* topic() const
        {
            return m_topic;
        }

    private:
        uint64_t     m_value {0};
        const Topic* m_topic {nullptr};
        std::string  m_name;
        bool         m_changed {false};
    };

    SystemStatistics(const std::string& brokerVersion, STopicManager topicManager, SSubscriptionManager subscriptionManager);
    ~SystemStatistics();

    void clear();

    /**
     * Register data size received by the broker
     * @param dataSize          Received data size
     * @param anyMessages       Received any messages count
     * @param publishedMessages Received publish messages count
     */
    /// Called once a second by the metrics scan, if set. Reading a queue's size takes the same
    /// mutex its push and pop take, so this must not be called from the message path.
    void setQueueDepthProvider(std::function<QueueDepths()> provider);

    void registerReceivedData(size_t dataSize, size_t anyMessages, size_t publishedMessages);

    /**
     * Register data size sent by the broker
     * @param dataSize          Sent data bytes
     * @param anyMessages       Sent any message count
     * @param publishedMessages Sent publish message count
     */
    void registerSentData(size_t dataSize, size_t anyMessages, size_t publishedMessages);
    void registerConnectedClient(bool existingSessionIsConnected, bool existingSessionIsClean);
    void registerDisconnectedClient(bool isCleanSession);

    SendReceiveResult receivedCounters();
    SendReceiveResult sentCounters();

    uint64_t getValue(SysTopicKind topicKind) const;

    /**
     * @brief How long this server has been serving, in seconds.
     *
     * Computed on demand rather than read from the BrokerUptime metric: that metric carries the
     * formatted "H:MM:SS" text published to $SYS, and giving it a numeric value as well would
     * have the scanning loop publish the raw seconds to the same topic a moment before the
     * formatted form, alternating between the two.
     *
     * @return seconds since this server started.
     */
    [[nodiscard]] uint64_t brokerUptimeSeconds() const;
    void                   setValue(SysTopicKind topicKind, uint64_t value);
    void                   increment(SysTopicKind topicKind, uint64_t value = 1);
    void                   decrement(SysTopicKind topicKind, uint64_t value = 1);

    static std::string sysTopicKindToString(SysTopicKind topicKind);
    static std::string sysTopicKindToTopic(SysTopicKind topicKind);

private:
    struct alignas(64) TrafficCounters
    {
        std::atomic<uint64_t> bytes {0};
        std::atomic<uint64_t> messages {0};
        std::atomic<uint64_t> published {0};
    };

    mutable std::mutex   m_mutex;
    TrafficCounters      m_received;
    TrafficCounters      m_sent;
    std::function<QueueDepths()> m_queueDepthProvider; ///< Supplied by the broker; see setQueueDepthProvider().
    std::vector<Metric>  m_metrics;
    STopicManager        m_topicManager;
    SSubscriptionManager m_subscriptionManager;
    std::thread          m_scanMetricsThread;
    sptk::Flag           m_scanMetricsTerminated;
    sptk::DateTime       m_brokerStartTime;
    const std::string    m_brokerVersion;

    Metric& metric(SysTopicKind topicKind)
    {
        return m_metrics[static_cast<size_t>(topicKind)];
    }

    const Metric& metric(SysTopicKind topicKind) const
    {
        return m_metrics[static_cast<size_t>(topicKind)];
    }

    std::atomic<uint64_t>* trafficCounter(SysTopicKind topicKind);
    const std::atomic<uint64_t>* trafficCounter(SysTopicKind topicKind) const;
    void syncTrafficMetrics();

    void scanMetricsThreadFunction();
};

using SSystemStatistics = std::shared_ptr<SystemStatistics>;

} // namespace xmq
