/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "SystemStatistics.h"

#include "common/mqtt/PublishMessage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

using enum SystemStatistics::SysTopicKind;

namespace {
const map<SystemStatistics::SysTopicKind, string> systemTopicNames = {
    {BrokerLoadBytesReceived, "$SYS/broker/load/bytes/received"},
    {BrokerLoadBytesSent, "$SYS/broker/load/bytes/sent"},
    {BrokerClientsConnected, "$SYS/broker/clients/connected"},
    {BrokerClientsDisconnected, "$SYS/broker/clients/disconnected"},
    {BrokerClientsMaximum, "$SYS/broker/clients/maximum"},
    {BrokerClientsTotal, "$SYS/broker/clients/total"},
    {BrokerMessagesReceived, "$SYS/broker/messages/received"},
    {BrokerMessagesSent, "$SYS/broker/messages/sent"},
    {BrokerMessagesPublishDropped, "$SYS/broker/messages/publish/dropped"},
    {BrokerMessagesPublishReceived, "$SYS/broker/messages/publish/received"},
    {BrokerMessagesPublishSent, "$SYS/broker/messages/publish/sent"},
    {BrokerMessagesRetainedCount, "$SYS/broker/messages/retained/count"},
    {BrokerQueuesReceive, "$SYS/broker/queues/receive/length"},
    {BrokerQueuesSend, "$SYS/broker/queues/send/length"},
    {BrokerQueuesDelivery, "$SYS/broker/queues/delivery/length"},
    {BrokerMessagesStored, "$SYS/broker/messages/stored"},
    {BrokerSubscriptionsCount, "$SYS/broker/subscriptions/count"},
    {BrokerTime, "$SYS/broker/time"},
    {BrokerUptime, "$SYS/broker/uptime"},
    {BrokerVersion, "$SYS/broker/version"},
};
}

SystemStatistics::Metric::Metric(const Topic* topic)
    : m_topic(topic)
{
}

SystemStatistics::Metric::Metric(const STopicManager& topic_manager, const SysTopicKind sysTopicKind, const string& topicPath)
{
    m_topic = topic_manager->getTopic(topicPath);
    m_name = sysTopicKindToString(sysTopicKind);
}

SystemStatistics::SystemStatistics(const std::string& brokerVersion, STopicManager topicManager, SSubscriptionManager subscriptionManager)
    : m_topicManager(std::move(topicManager))
    , m_subscriptionManager(std::move(subscriptionManager))
    , m_brokerStartTime(DateTime::Now())
    , m_brokerVersion(brokerVersion)
{
    m_metrics.resize(static_cast<size_t>(SysTopicKind::Max));
    for (const auto& [sysTopicKind, topicPath]: systemTopicNames)
    {
        Metric metric(m_topicManager, sysTopicKind, topicPath);
        m_metrics[static_cast<int>(sysTopicKind)] = std::move(metric);
    }

    m_scanMetricsThread = thread([this]
                                  {
                                      scanMetricsThreadFunction();
                                  });
}

SystemStatistics::~SystemStatistics()
{
    m_scanMetricsTerminated.set(true);
    // joinable(), because the thread is only started when metrics scanning is switched on.
    if (m_scanMetricsThread.joinable())
    {
        m_scanMetricsThread.join();
    }
}

void SystemStatistics::setQueueDepthProvider(std::function<QueueDepths()> provider)
{
    std::scoped_lock lock(m_mutex);
    m_queueDepthProvider = std::move(provider);
}

void SystemStatistics::registerReceivedData(const size_t dataSize, const size_t anyMessages, const size_t publishedMessages)
{
    m_received.bytes.fetch_add(dataSize, memory_order_relaxed);
    m_received.messages.fetch_add(anyMessages, memory_order_relaxed);
    m_received.published.fetch_add(publishedMessages, memory_order_relaxed);
}

void SystemStatistics::registerSentData(const size_t dataSize, const size_t anyMessages, const size_t publishedMessages)
{
    m_sent.bytes.fetch_add(dataSize, memory_order_relaxed);
    m_sent.messages.fetch_add(anyMessages, memory_order_relaxed);
    m_sent.published.fetch_add(publishedMessages, memory_order_relaxed);
}

void SystemStatistics::registerConnectedClient(const bool existingSessionIsConnected, const bool existingSessionIsClean)
{
    std::scoped_lock lock(m_mutex);
    if (existingSessionIsConnected)
    {
        return;
    }

    auto& clientsDisconnected = metric(BrokerClientsDisconnected);
    if (!existingSessionIsClean)
    {
        clientsDisconnected -= 1;
    }

    auto& clientsConnected = metric(BrokerClientsConnected);
    auto& clientsMaximum = metric(BrokerClientsMaximum);
    auto& clientsTotal = metric(BrokerClientsTotal);

    clientsConnected += 1;
    if (clientsConnected.getValue() > clientsMaximum.getValue())
    {
        clientsMaximum.setValue(clientsConnected.getValue());
    }

    clientsTotal = clientsConnected.getValue() + clientsDisconnected.getValue();
}

void SystemStatistics::registerDisconnectedClient(const bool isCleanSession)
{
    std::scoped_lock lock(m_mutex);
    auto&            clientsConnected = metric(BrokerClientsConnected);
    auto&            clientsDisconnected = metric(BrokerClientsDisconnected);
    if (!isCleanSession)
    {
        clientsDisconnected += 1;
    }
    clientsConnected -= 1;
    metric(BrokerClientsTotal) = clientsConnected.getValue() + clientsDisconnected.getValue();
}

SendReceiveResult SystemStatistics::receivedCounters()
{
    return {
        m_received.bytes.load(memory_order_relaxed),
        m_received.messages.load(memory_order_relaxed),
        m_received.published.load(memory_order_relaxed)};
}

SendReceiveResult SystemStatistics::sentCounters()
{
    return {
        m_sent.bytes.load(memory_order_relaxed),
        m_sent.messages.load(memory_order_relaxed),
        m_sent.published.load(memory_order_relaxed)};
}

void SystemStatistics::increment(const SysTopicKind topicKind, const uint64_t value)
{
    std::scoped_lock lock(m_mutex);
    metric(topicKind) += value;
}

void SystemStatistics::decrement(const SysTopicKind topicKind, const uint64_t value)
{
    std::scoped_lock lock(m_mutex);
    auto&            metr = metric(topicKind);
    if (metr.getValue() == 0)
    {
        throw Exception("The " + sysTopicKindToString(topicKind) + " counter is already 0");
    }
    metr -= value;
}

uint64_t SystemStatistics::getValue(const SysTopicKind topicKind) const
{
    if (const auto* counter = trafficCounter(topicKind))
    {
        return counter->load(memory_order_relaxed);
    }
    std::scoped_lock lock(m_mutex);
    return metric(topicKind).getValue();
}

void SystemStatistics::setValue(const SysTopicKind topicKind, const uint64_t value)
{
    if (auto* counter = trafficCounter(topicKind))
    {
        counter->store(value, memory_order_relaxed);
        return;
    }
    std::scoped_lock lock(m_mutex);
    metric(topicKind) = value;
}

std::string SystemStatistics::sysTopicKindToString(const SysTopicKind topicKind)
{
    switch (topicKind)
    {
        case BrokerLoadBytesReceived:
            return "BrokerLoadBytesReceived";
        case BrokerLoadBytesSent:
            return "BrokerLoadBytesSent";
        case BrokerClientsConnected:
            return "BrokerClientsConnected";
        case BrokerClientsDisconnected:
            return "BrokerClientsDisconnected";
        case BrokerClientsMaximum:
            return "BrokerClientsMaximum";
        case BrokerClientsTotal:
            return "BrokerClientsTotal";
        case BrokerMessagesReceived:
            return "BrokerMessagesReceived";
        case BrokerMessagesSent:
            return "BrokerMessagesSent";
        case BrokerMessagesPublishDropped:
            return "BrokerMessagesPublishDropped";
        case BrokerMessagesPublishReceived:
            return "BrokerMessagesPublishReceived";
        case BrokerMessagesPublishSent:
            return "BrokerMessagesPublishSent";
        case BrokerMessagesRetainedCount:
            return "BrokerMessagesRetainedCount";
        case BrokerMessagesStored:
            return "BrokerMessagesStored";
        case BrokerSubscriptionsCount:
            return "BrokerSubscriptionsCount";
        case BrokerQueuesReceive:
            return "BrokerQueuesReceive";
        case BrokerQueuesSend:
            return "BrokerQueuesSend";
        case BrokerQueuesDelivery:
            return "BrokerQueuesDelivery";
        case BrokerTime:
            return "BrokerTime";
        case BrokerUptime:
            return "BrokerUptime";
        case BrokerVersion:
            return "BrokerVersion";
        case Max:
            break;
    }
    return {};
}

string SystemStatistics::sysTopicKindToTopic(const SysTopicKind topicKind)
{
    const auto it = systemTopicNames.find(topicKind);
    if (it == systemTopicNames.end())
    {
        throw Exception("Unknown system topic kind");
    }
    return it->second;
}

void SystemStatistics::scanMetricsThreadFunction()
{
    const auto&           brokerVersionMetric = m_metrics[static_cast<int>(BrokerVersion)];
    mqtt::PublishMessage* messagePtr = new mqtt::PublishMessage(brokerVersionMetric.topic(), string_view(m_brokerVersion), 0, true);
    m_subscriptionManager->publishMessage(shared_ptr<mqtt::PublishMessage>(messagePtr), MessageDomain::Server);

    while (!m_scanMetricsTerminated.wait_for(true, 1s))
    {
        std::scoped_lock lock(m_mutex);

        syncTrafficMetrics();

        // Once a second, and only here: size() takes each queue's own mutex - the one its push and
        // pop take - and these queues carry every message the broker handles.
        if (m_queueDepthProvider)
        {
            const auto depths = m_queueDepthProvider();
            metric(BrokerQueuesReceive) = depths.receive;
            metric(BrokerQueuesSend) = depths.send;
            metric(BrokerQueuesDelivery) = depths.delivery;
        }

        for (auto& metric: m_metrics)
        {
            if (metric.isChanged())
            {
                messagePtr = new mqtt::PublishMessage(metric.topic(), string_view(to_string(metric.readValue())), 0, true);
                m_subscriptionManager->publishMessage(shared_ptr<mqtt::PublishMessage>(messagePtr), MessageDomain::Server);
            }
        }

        auto&        brokerUpTimeMetric = m_metrics[static_cast<int>(BrokerUptime)];
        const auto   brokerUpTime = chrono::duration_cast<chrono::seconds>(DateTime::Now() - m_brokerStartTime).count();
        const auto   hours = brokerUpTime / 3600;
        const auto   minutes = brokerUpTime % 3600 / 60;
        const auto   seconds = brokerUpTime % 60;
        stringstream stream;
        stream << hours << ":" << setfill('0') << setw(2) << minutes << ":" << setw(2) << seconds;
        messagePtr = new mqtt::PublishMessage(brokerUpTimeMetric.topic(), string_view(stream.str()), 0, true);
        m_subscriptionManager->publishMessage(shared_ptr<mqtt::PublishMessage>(messagePtr), MessageDomain::Server);

        auto& brokerTimeMetric = m_metrics[static_cast<int>(BrokerTime)];
        messagePtr = new mqtt::PublishMessage(brokerTimeMetric.topic(), string_view(DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS)), 0, true);
        m_subscriptionManager->publishMessage(shared_ptr<mqtt::PublishMessage>(messagePtr), MessageDomain::Server);
    }
}

uint64_t SystemStatistics::brokerUptimeSeconds() const
{
    const auto seconds = chrono::duration_cast<chrono::seconds>(DateTime::Now() - m_brokerStartTime).count();
    return seconds > 0 ? static_cast<uint64_t>(seconds) : 0;
}

void SystemStatistics::clear()
{
    m_received.bytes.store(0, memory_order_relaxed);
    m_received.messages.store(0, memory_order_relaxed);
    m_received.published.store(0, memory_order_relaxed);
    m_sent.bytes.store(0, memory_order_relaxed);
    m_sent.messages.store(0, memory_order_relaxed);
    m_sent.published.store(0, memory_order_relaxed);

    std::scoped_lock lock(m_mutex);
    for (auto& metric: m_metrics)
    {
        metric = 0;
    }
}

atomic<uint64_t>* SystemStatistics::trafficCounter(const SysTopicKind topicKind)
{
    switch (topicKind)
    {
        case BrokerLoadBytesReceived: return &m_received.bytes;
        case BrokerMessagesReceived: return &m_received.messages;
        case BrokerMessagesPublishReceived: return &m_received.published;
        case BrokerLoadBytesSent: return &m_sent.bytes;
        case BrokerMessagesSent: return &m_sent.messages;
        case BrokerMessagesPublishSent: return &m_sent.published;
        default: return nullptr;
    }
}

const atomic<uint64_t>* SystemStatistics::trafficCounter(const SysTopicKind topicKind) const
{
    return const_cast<SystemStatistics*>(this)->trafficCounter(topicKind);
}

void SystemStatistics::syncTrafficMetrics()
{
    const auto sync = [this](const SysTopicKind kind, const atomic<uint64_t>& counter) {
        const auto value = counter.load(memory_order_relaxed);
        if (metric(kind).getValue() != value)
        {
            metric(kind) = value;
        }
    };

    sync(BrokerLoadBytesReceived, m_received.bytes);
    sync(BrokerMessagesReceived, m_received.messages);
    sync(BrokerMessagesPublishReceived, m_received.published);
    sync(BrokerLoadBytesSent, m_sent.bytes);
    sync(BrokerMessagesSent, m_sent.messages);
    sync(BrokerMessagesPublishSent, m_sent.published);
}
